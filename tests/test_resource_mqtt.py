#!/usr/bin/env python3
"""Exercise four RAM sessions with a 63-byte account on a local broker."""
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
        # Existing command subscription authorizes all four sessions; no peer
        # subscriptions, pairing list or MQTT read replies are needed.
        for index, peer in enumerate(peers + ['000000000005']):
            rid = f'grant-{index}'
            publish(base + '/command', {'request_id': rid, 'action': 'sensor.session.set',
                    'params': {'v': 2, 'a1': peer, 'boot': 1, 'token': 'ab' * 16}})
            wait(lambda: any(t == base + '/result' and json.loads(p).get('request_id') == rid
                             for t,p,r in records))
            value = next(json.loads(p) for t,p,r in reversed(records)
                         if t == base + '/result' and json.loads(p).get('request_id') == rid)
            assert value['code'] == ('accepted' if index < 4 else 'busy')
        worker.stdin.write(json.dumps({'action':'binding.status.get','params':{}}).encode()+b'\n')
        worker.stdin.flush()
        assert select.select([worker.stdout],[],[],15)[0]
        status=json.loads(worker.stdout.readline())['data']
        assert status['max_sessions']==4 and len(status['peers'])==4
        assert {p['mac'] for p in status['peers']}==set(peers)
        request = {'request_id': 'long-account-status', 'action': 'status.get'}
        publish(base + '/command', request)
        wait(lambda: any(t == base + '/result' and json.loads(p).get('request_id') == request['request_id']
                         for t, p, r in records))
        assert all(not retained for topic, payload, retained in records
                   if topic.endswith('/response') or topic == base + '/result')
        print('PASS M1 rename results before reconnect, four RAM sessions, fifth rejected, long account and native command results')
    finally:
        cleanup(client, [worker], failing=sys.exc_info()[0] is not None)


if __name__ == '__main__':
    main(int(sys.argv[1]))
