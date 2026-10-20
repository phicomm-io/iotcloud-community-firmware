#!/usr/bin/env python3
"""Exercise all four M1 subscriptions with a 63-byte account on a local broker."""
import json
from pathlib import Path
import select
import subprocess
import sys
import time

from mqtt_observer import Observer, cleanup


def main(port):
    root = Path(__file__).resolve().parents[1]
    user = 'r' * 63
    key = 'u_' + user.encode().hex()
    identity = 'aabbccddeeff'
    base = f'phicomm/v2/{key}/m1/{identity}'
    peers = [f'{i:012x}' for i in range(1, 5)]
    records = []
    client = Observer(port, lambda c, u, m: records.append((m.topic, m.payload.decode(), m.retain))).start(f'phicomm/v2/{key}/#')
    worker = None

    def wait(predicate, seconds=15):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            client.check()
            if predicate():
                client.check()
                return
            assert worker.poll() is None, 'M1 simulator exited'
            time.sleep(.03)
        raise AssertionError('M1 resource MQTT timeout')

    def publish(topic, payload):
        client.publish(topic, json.dumps(payload))

    def reply(peer, operation, request_id):
        topic = f'phicomm/v2/{key}/a1/{peer}/link/{identity}/response'
        payload = {'v': 1, 'src': peer, 'dst': identity, 'boot': 1,
                   'id': request_id, 'revision': 1, 'op': operation, 'token': 'ab' * 16}
        start = len(records)
        deadline = time.monotonic() + 15
        # Subscriptions are installed during the reconnect after binding.set.
        # A missed request may be retried; each attempt has a unique ID.
        while time.monotonic() < deadline:
            payload['id'] += 1
            publish(f'{base}/link/{peer}/request', payload)
            sent_id = payload['id']
            for _ in range(10):
                client.check()
                matches = [json.loads(p) for t, p, r in records[start:]
                           if t == topic and json.loads(p).get('id') == sent_id]
                if matches:
                    response = matches[-1]
                    assert response['src'] == identity and response['dst'] == peer
                    assert response['token'] == payload['token'] and response['ip'] == '127.0.0.1'
                    assert response['op'] == ('bound' if operation == 'bind' else 'sample')
                    return response
                assert worker.poll() is None
                time.sleep(.03)
        raise AssertionError(f'No {operation} reply from subscription {peer}')

    try:
        worker = subprocess.Popen([str(root / 'build/simulator/M1'), identity,
                                   '127.0.0.1', '127.0.0.1', str(port), user],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        wait(lambda: any(t == base + '/sensor' for t, p, r in records))
        # Renaming changes the MQTT session/HA discovery. Its result must be
        # flushed on the original connection before reconnect clears the queue.
        for index in range(3):
            request_id = f'rename-before-reconnect-{index}'
            start = len(records)
            publish(base + '/command', {'request_id': request_id, 'action': 'config.set',
                                       'params': {'section': 'device',
                                                  'values': {'name': f'M1 MQTT QA {index}'}}})
            wait(lambda: any(t == base + '/result' and json.loads(p).get('request_id') == request_id
                             for t, p, r in records[start:]))
            result_index = next(i for i in range(start, len(records))
                                if records[i][0] == base + '/result'
                                and json.loads(records[i][1]).get('request_id') == request_id)
            assert json.loads(records[result_index][1])['code'] == 'accepted'
            wait(lambda: any(t == base + '/availability' and p == 'online'
                             for t, p, r in records[result_index + 1:]))
            offline = next(i for i in range(start, len(records))
                           if records[i][0] == base + '/availability' and records[i][1] == 'offline')
            assert result_index < offline, 'rename result lost until after reconnect'
        command = {'action': 'binding.set',
                   'params': {'response_ids': [f'{peer}:{identity}' for peer in peers]}}
        worker.stdin.write(json.dumps(command).encode() + b'\n')
        worker.stdin.flush()
        assert select.select([worker.stdout], [], [], 15)[0], 'binding setup timeout'
        assert json.loads(worker.stdout.readline())['code'] == 'accepted'
        # A fifth peer is rejected before storage/subscription changes.
        command['params']['response_ids'].append(f'000000000005:{identity}')
        worker.stdin.write(json.dumps(command).encode() + b'\n')
        worker.stdin.flush()
        assert select.select([worker.stdout], [], [], 15)[0], 'fifth-peer rejection timeout'
        assert json.loads(worker.stdout.readline())['code'] == 'invalid_params'
        for index, peer in enumerate(peers):
            reply(peer, 'bind', index * 1000)
            sample = reply(peer, 'read', index * 1000 + 100)
            assert sample['body']['sequence'] > 0 and sample['body']['valid']
        request = {'request_id': 'long-account-status', 'action': 'status.get'}
        publish(base + '/command', request)
        wait(lambda: any(t == base + '/result' and json.loads(p).get('request_id') == request['request_id']
                         for t, p, r in records))
        assert all(not retained for topic, payload, retained in records
                   if topic.endswith('/response') or topic == base + '/result')
        print('PASS M1 rename results before reconnect, four actual MQTT subscriptions, long account, bind/sample routing, IP snapshot and command result')
    finally:
        cleanup(client, [worker], failing=sys.exc_info()[0] is not None)


if __name__ == '__main__':
    main(int(sys.argv[1]))
