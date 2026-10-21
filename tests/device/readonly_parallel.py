"""Process isolation and bounded cancellation for read-only device checks."""
import multiprocessing
import ctypes
import os
import signal
import time


CANCEL_JOIN_SECONDS = 45
TERMINATE_JOIN_SECONDS = 3
KILL_JOIN_SECONDS = 5
STOP_POLL_SECONDS = .02


class SharedStop:
    """Lock-free byte flag for Linux/POSIX forked workers only."""

    def __init__(self, value):
        self._value = value

    def set(self):
        self._value.value = 1

    def is_set(self):
        return bool(self._value.value)

    def wait(self, timeout=None):
        deadline = None if timeout is None else time.monotonic() + max(0, timeout)
        while not self.is_set():
            if deadline is None:
                time.sleep(STOP_POLL_SECONDS)
            else:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    return self.is_set()
                time.sleep(min(STOP_POLL_SECONDS, remaining))
        return True


def _save_runner(runner):
    try:
        runner.save()
    finally:
        save_scheduling = getattr(runner, 'save_scheduling', None)
        if save_scheduling is not None:
            save_scheduling()


def _worker(runner, execute, stop):
    signal.signal(signal.SIGINT, signal.SIG_DFL)
    signal.signal(signal.SIGTERM, signal.SIG_DFL)
    signal.pthread_sigmask(signal.SIG_UNBLOCK, {signal.SIGINT, signal.SIGTERM})
    runner.stop = stop
    try:
        execute(runner)
        if stop.is_set() and not runner.blockers:
            runner.block('', 'parallel run cancelled by another device')
            _save_runner(runner)
            raise SystemExit(1)
    except BaseException as exc:
        stop.set()
        if not runner.blockers:
            runner.block('', 'parallel worker failed: ' + type(exc).__name__)
        try:
            _save_runner(runner)
        except BaseException:
            pass
        raise SystemExit(1) from None


def run_parallel(runners, execute, workers):
    """Run initialized single-target runners under one caller-owned flock.

    This controller requires Linux/POSIX ``fork`` and a shared atomic byte;
    it is not a portable multiprocessing abstraction.
    """
    if workers != 3 or not 1 < len(runners) <= 3:
        raise ValueError('parallel mode requires up to three explicit target workers')
    context = multiprocessing.get_context('fork')
    stop = SharedStop(context.RawValue(ctypes.c_ubyte, 0))
    processes = [context.Process(target=_worker, args=(runner, execute, stop))
                 for runner in runners]
    previous_handlers = {}
    signal_state = {'requested': False, 'cleaning': False}

    def request_stop(signum, frame):
        if signal_state['requested'] or signal_state['cleaning']:
            return
        signal_state['requested'] = True
        raise KeyboardInterrupt('parallel device workers interrupted')

    for signum in (signal.SIGINT, signal.SIGTERM):
        previous_handlers[signum] = signal.signal(signum, request_stop)

    started = []
    pids = []
    failure = None
    cancel_deadline = None
    try:
        for process in processes:
            previous_mask = signal.pthread_sigmask(
                signal.SIG_BLOCK, {signal.SIGINT, signal.SIGTERM})
            try:
                process.start()
                started.append(process)
            finally:
                signal.pthread_sigmask(signal.SIG_SETMASK, previous_mask)
        pids = [process.pid for process in started]

        pending = set(started)
        while pending:
            for process in tuple(pending):
                process.join(.05)
                if process.exitcode is not None:
                    pending.remove(process)
                    if process.exitcode != 0 and failure is None:
                        failure = 'device worker exited with status ' + str(process.exitcode)
                        stop.set()
            if stop.is_set() and cancel_deadline is None:
                cancel_deadline = time.monotonic() + CANCEL_JOIN_SECONDS
            if cancel_deadline is not None and time.monotonic() >= cancel_deadline:
                failure = failure or 'device workers exceeded cancellation deadline'
                for process in pending:
                    if process.is_alive():
                        process.terminate()
                terminate_deadline = time.monotonic() + TERMINATE_JOIN_SECONDS
                for process in pending:
                    process.join(max(0, terminate_deadline-time.monotonic()))
                for process in pending:
                    if process.is_alive() and hasattr(process, 'kill'):
                        process.kill()
                kill_deadline = time.monotonic() + KILL_JOIN_SECONDS
                for process in pending:
                    process.join(max(0, kill_deadline-time.monotonic()))
                # Keep the caller's global flock until every worker is dead.
                for process in pending:
                    while process.is_alive():
                        process.join(1)
                    process.join()
                pending.clear()

        if stop.is_set() and failure is None:
            failure = 'parallel stop requested before all workers completed'
    except BaseException:
        signal_state['cleaning'] = True
        stop.set()
        for process in started:
            if process.is_alive():
                process.terminate()
        for process in started:
            process.join(TERMINATE_JOIN_SECONDS)
            if process.is_alive() and hasattr(process, 'kill'):
                process.kill()
        for process in started:
            while process.is_alive():
                process.join(1)
            process.join()
        raise
    finally:
        for signum, handler in previous_handlers.items():
            signal.signal(signum, handler)
        for process in started:
            process.close()

    return {'completed': failure is None, 'failure': failure,
            'worker_pids': pids}


def merge_json_files(paths, output_path, *, list_keys, dict_keys=(), metadata=None):
    """Merge isolated per-device JSON while retaining the established schema."""
    import json
    from pathlib import Path

    documents = []
    for path in paths:
        with open(path, encoding='utf-8') as stream:
            documents.append(json.load(stream))
    if not documents:
        raise ValueError('no device evidence to merge')
    merged = dict(documents[0])
    for key in list_keys:
        merged[key] = [item for document in documents for item in document.get(key, [])]
    for key in dict_keys:
        merged[key] = {}
        for document in documents:
            merged[key].update(document.get(key, {}))
    merged['targets'] = [item for document in documents
                         for item in document.get('targets', [])]
    if metadata:
        merged['parallel_execution'] = metadata
    output_path = Path(output_path)
    write_json_atomic(merged, output_path)
    return merged


def write_json_atomic(data, output_path):
    import json
    from pathlib import Path

    output_path = Path(output_path)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    temporary = output_path.with_name(output_path.name + '.parallel-tmp-' + str(os.getpid()))
    fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        with os.fdopen(fd, 'w', encoding='utf-8') as stream:
            json.dump(data, stream, ensure_ascii=False, indent=2)
            stream.write('\n')
        os.replace(temporary, output_path)
    except BaseException:
        try:
            os.unlink(temporary)
        except OSError:
            pass
        raise


def has_complete_targets_and_baselines(evidence, expected_ips, baseline_key):
    expected = set(expected_ips)
    declared = []
    for target in evidence.get('targets', []):
        if isinstance(target, (list, tuple)) and target:
            declared.append(target[0])
        elif isinstance(target, dict):
            declared.append(target.get('ip'))
    baseline = evidence.get(baseline_key)
    return (len(declared) == len(expected) and set(declared) == expected and
            isinstance(baseline, dict) and set(baseline) == expected)


def covers_expected_records(records, expected_keys, fields, *, exact=False):
    expected = set(expected_keys)
    actual = [tuple(record.get(field) for field in fields) for record in records]
    return (expected <= set(actual) and
            (not exact or (len(actual) == len(expected) and set(actual) == expected)))


def covers_recovery_targets(records, expected_pairs):
    recovered = set()
    for record in records:
        if record.get('completed') is not True:
            continue
        for ip, result in record.get('targets', {}).items():
            if (isinstance(result, dict) and result.get('completed') is True and
                    result.get('consecutive_successes', 0) >= 3):
                recovered.add((ip, record.get('name')))
    return set(expected_pairs) <= recovered
