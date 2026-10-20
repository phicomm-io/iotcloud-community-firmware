"""Controlled lifecycle failures; no broker, worker, retries or timeout relaxation."""
import contextlib
import io
import sys
import threading
import unittest
from types import SimpleNamespace

from tests.mqtt_observer import Observer, cleanup


class FakeClient:
    def __init__(self):
        self.connack = 0
        self.suback = [1]
        self.subscribe_rc = 0
        self.publish_rc = 0
        self.ack = True
        self.disconnected = self.stopped = False
        self.cleanup_error = False
        self.connect_delay = False
        self.connected_callback = False

    def connect(self, host, port):
        return 0

    def loop_start(self):
        def connected():
            self.connected_callback = True
            self.on_connect(self, None, None, self.connack, None)
        if self.connect_delay:
            self.timer = threading.Timer(.01, connected)
            self.timer.start()
        else:
            connected()
        return 0

    def subscribe(self, topic, qos):
        assert self.connected_callback, 'subscribe before CONNACK'
        if self.suback is not None:
            # Deliberately deliver SUBACK before subscribe returns its mid.
            self.on_subscribe(self, None, 7, self.suback, None)
        return self.subscribe_rc, 7

    def publish(self, *args, **kwargs):
        return SimpleNamespace(rc=self.publish_rc, is_published=lambda: self.ack)

    def disconnect(self):
        self.disconnected = True
        self.on_disconnect(self, None, None, 0, None)
        if self.cleanup_error:
            raise RuntimeError('cleanup')

    def loop_stop(self):
        self.stopped = True
        if hasattr(self, 'timer'):
            self.timer.join()


class ObserverLifecycle(unittest.TestCase):
    def make(self):
        client = FakeClient()
        return Observer(12345, lambda *a: None, timeout=.04, client=client), client

    def test_waits_for_connack_and_accepts_early_suback(self):
        observer, client = self.make()
        client.connect_delay = True
        observer.start('#')
        observer.publish('synthetic', 'test')
        observer.close()
        self.assertTrue(client.disconnected and client.stopped)
        observer.check()  # intentional disconnect is not a failure

    def test_connack_rejected_and_suback_missing_or_rejected(self):
        for field, value, message in (
            ('connack', 5, 'CONNACK rejected'),
            ('suback', None, 'timeout'),
            ('suback', [128], 'SUBACK'),
            ('suback', [0], 'QoS1'),
            ('subscribe_rc', 4, 'subscribe rc=4'),
        ):
            with self.subTest(field=field, value=value):
                observer, client = self.make()
                setattr(client, field, value)
                with self.assertRaisesRegex(AssertionError, message):
                    observer.start('#')
                self.assertTrue(client.disconnected and client.stopped)

    def test_missing_connack_is_bounded(self):
        observer, client = self.make()
        client.loop_start = lambda: 0
        with self.assertRaisesRegex(AssertionError, 'timeout'):
            observer.start('#')
        self.assertTrue(client.disconnected and client.stopped)

    def test_disconnect_is_sticky_even_when_predicate_true(self):
        observer, client = self.make()
        observer.start('#')
        client.on_disconnect(client, None, None, 7, None)
        client.on_connect(client, None, None, 0, None)
        with self.assertRaisesRegex(AssertionError, 'unexpected disconnect'):
            observer.wait(lambda: True)
        observer.close(failing=True)

    def test_initial_disconnect_fails_before_subscribe(self):
        observer, client = self.make()
        client.loop_start = lambda: client.on_disconnect(client, None, None, 7, None) or 0
        with self.assertRaisesRegex(AssertionError, 'unexpected disconnect'):
            observer.start('#')

    def test_publish_no_connection_and_missing_ack(self):
        for field, value, message in (('publish_rc', 4, 'publish rc=4'),
                                      ('ack', False, 'timeout')):
            with self.subTest(field=field):
                observer, client = self.make()
                observer.start('#')
                setattr(client, field, value)
                with self.assertRaisesRegex(AssertionError, message):
                    observer.publish('synthetic', 'test')
                observer.close(failing=True)

    def test_message_callback_failure_is_not_silent(self):
        observer, client = self.make()
        observer.start('#')
        def broken(*args):
            raise ValueError('synthetic')
        observer._on_message = broken
        client.on_message(client, None, None)
        with self.assertRaisesRegex(AssertionError, 'message callback failed: ValueError'):
            observer.check()
        observer.close(failing=True)

    def test_cleanup_preserves_original_exception_and_stops_loop(self):
        observer, client = self.make()
        observer.start('#')
        client.cleanup_error = True
        original = ValueError('original scenario failure')
        with contextlib.redirect_stderr(io.StringIO()) as log:
            try:
                try:
                    raise original
                finally:
                    observer.close(failing=sys.exc_info()[0] is not None)
            except ValueError as exc:
                self.assertIs(exc, original)
            else:
                self.fail('original exception lost')
        self.assertTrue(client.stopped)
        self.assertIn('cleanup failed', log.getvalue())

    def test_worker_cleanup_preserves_scenario_error_and_closes_observer(self):
        observer, client = self.make()
        observer.start('#')
        worker = SimpleNamespace(poll=lambda: None,
                                 terminate=lambda: (_ for _ in ()).throw(RuntimeError('worker cleanup')))
        original = ValueError('scenario')
        with contextlib.redirect_stderr(io.StringIO()):
            try:
                try:
                    raise original
                finally:
                    cleanup(observer, [worker], failing=sys.exc_info()[0] is not None)
            except ValueError as exc:
                self.assertIs(exc, original)
            else:
                self.fail('original exception lost')
        self.assertTrue(client.disconnected and client.stopped)

    def test_late_disconnect_is_not_hidden_by_close(self):
        observer, client = self.make()
        observer.start('#')
        client.on_disconnect(client, None, None, 7, None)
        client.cleanup_error = True
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaisesRegex(AssertionError, 'unexpected disconnect'):
            observer.close()
        self.assertTrue(client.stopped)


if __name__ == '__main__':
    unittest.main()
