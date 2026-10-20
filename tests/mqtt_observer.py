"""Strict, bounded observer for isolated host MQTT tests; never reconnect silently."""
import sys
import threading
import time

import paho.mqtt.client as mqtt


class Observer:
    def __init__(self, port, on_message, *, timeout=12, client=None):
        self.client = client or mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                                            reconnect_on_failure=False)
        self.port, self.timeout = port, timeout
        self._lock = threading.Lock()
        self._failure = None
        self._connected = False
        self._closing = False
        self._subacks = {}
        self._on_message = on_message
        self.client.on_connect = self._connect
        self.client.on_disconnect = self._disconnect
        self.client.on_subscribe = self._subscribe
        self.client.on_message = self._message

    def _fail(self, reason):
        with self._lock:
            if self._failure is None:
                self._failure = reason

    def _connect(self, client, userdata, flags, reason, properties):
        if reason != 0:
            self._fail('CONNACK rejected: ' + str(reason))
        else:
            with self._lock:
                self._connected = True

    def _disconnect(self, client, userdata, flags, reason, properties):
        with self._lock:
            self._connected = False
            if not self._closing and self._failure is None:
                self._failure = 'unexpected disconnect: ' + str(reason)

    def _subscribe(self, client, userdata, mid, reasons, properties):
        # Store by mid: a fast SUBACK can precede subscribe() returning its mid.
        with self._lock:
            self._subacks[mid] = reasons

    def _message(self, client, userdata, message):
        try:
            self._on_message(client, userdata, message)
        except Exception as exc:
            self._fail('message callback failed: ' + type(exc).__name__)

    def check(self):
        with self._lock:
            failure = self._failure
        if failure is not None:
            raise AssertionError('MQTT observer ' + failure)

    def wait(self, predicate, seconds=None):
        deadline = time.monotonic() + (self.timeout if seconds is None else seconds)
        while True:
            self.check()
            if predicate():
                self.check()
                return
            if time.monotonic() >= deadline:
                raise AssertionError('MQTT observer timeout')
            time.sleep(.03)

    def _connection_ready(self):
        with self._lock:
            return self._connected

    def _subscription_ready(self, mid):
        with self._lock:
            return mid in self._subacks

    def start(self, topic):
        try:
            self.client.connect_timeout = self.timeout
            rc = self.client.connect('127.0.0.1', self.port)
            if rc != mqtt.MQTT_ERR_SUCCESS:
                raise AssertionError('MQTT observer connect rc=' + str(rc))
            rc = self.client.loop_start()
            if rc != mqtt.MQTT_ERR_SUCCESS:
                raise AssertionError('MQTT observer loop_start rc=' + str(rc))
            self.wait(self._connection_ready)
            rc, mid = self.client.subscribe(topic, 1)
            if rc != mqtt.MQTT_ERR_SUCCESS:
                raise AssertionError('MQTT observer subscribe rc=' + str(rc))
            self.wait(lambda: self._subscription_ready(mid))
            with self._lock:
                reasons = self._subacks.pop(mid)
            if len(reasons) != 1 or reasons[0] != 1:
                raise AssertionError('MQTT observer SUBACK did not grant QoS1: ' + str(reasons))
            self.check()
            return self
        except BaseException:
            self.close(failing=True)
            raise

    def publish(self, topic, payload, retain=False):
        self.check()
        info = self.client.publish(topic, payload, qos=1, retain=retain)
        def acknowledged():
            if info.rc != mqtt.MQTT_ERR_SUCCESS:
                raise AssertionError('MQTT observer publish rc=' + str(info.rc))
            return info.is_published()
        self.wait(acknowledged)

    def close(self, failing=False):
        # Atomically classify the last callback before marking our own shutdown.
        with self._lock:
            failure = self._failure
            self._closing = True
        first_error = None
        for action in (self.client.disconnect, self.client.loop_stop):
            try:
                action()
            except Exception as exc:
                if first_error is None:
                    first_error = exc
        if not failing:
            if failure is not None:
                raise AssertionError('MQTT observer ' + failure)
            if first_error is not None:
                raise first_error
        elif first_error is not None:
            print('MQTT observer cleanup failed: ' + type(first_error).__name__, file=sys.stderr)
