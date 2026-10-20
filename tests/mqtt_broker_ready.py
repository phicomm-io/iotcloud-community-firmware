"""Bounded startup probe for the anonymous, disposable host-test broker only."""
import socket
import sys
import time
import uuid


def probe(port, timeout=1):
    # One deadline for connect, CONNECT write, and the entire CONNACK frame.
    deadline = time.monotonic() + timeout
    client_id = ('phicomm-ready-' + uuid.uuid4().hex).encode('ascii')
    variable = b'\x00\x04MQTT\x04\x02\x00\x0a'
    body = variable + len(client_id).to_bytes(2, 'big') + client_id
    packet = b'\x10' + bytes([len(body)]) + body

    def remaining():
        seconds = deadline - time.monotonic()
        if seconds <= 0:
            raise TimeoutError('isolated broker handshake deadline')
        return seconds

    with socket.create_connection(('127.0.0.1', port), timeout=remaining()) as connection:
        connection.settimeout(remaining())
        connection.sendall(packet)
        reply = b''
        while len(reply) < 4:
            connection.settimeout(remaining())
            part = connection.recv(4 - len(reply))
            if not part:
                raise ConnectionError('isolated broker closed before CONNACK')
            reply += part
        # MQTT 3.1.1 CONNACK, clean-session means session-present must be zero.
        if reply[:3] != b'\x20\x02\x00' or reply[3] > 5:
            raise AssertionError('isolated broker malformed CONNACK: ' + reply.hex())
        if reply[3] != 0:
            raise AssertionError('isolated broker rejected CONNECT: ' + str(reply[3]))
        connection.settimeout(remaining())
        connection.sendall(b'\xe0\x00')  # No subscription, publish, or retained data.


def wait_for_broker(port, attempts=50, interval=.1, timeout=1):
    last = None
    for attempt in range(attempts):
        try:
            probe(port, timeout)
            return
        except OSError as exc:
            # Startup-only probes tolerate the Docker TCP proxy accepting before
            # its backend listens. Scenario observers never retry/reconnect.
            last = exc
        if attempt + 1 < attempts:
            time.sleep(interval)
    raise RuntimeError('isolated MQTT broker did not complete CONNACK: ' +
                       type(last).__name__) from last


if __name__ == '__main__':
    wait_for_broker(int(sys.argv[1]))
