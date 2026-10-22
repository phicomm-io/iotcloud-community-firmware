#!/usr/bin/env python3
"""Production M1 MQTT worker with synthetic BMP388 samples and isolated broker."""
import json
import select
import subprocess
import sys
import time
from pathlib import Path
from jinja2 import Environment, StrictUndefined
from mqtt_observer import Observer, cleanup


def main(port):
    records = []
    observer = Observer(port, lambda c, u, m: records.append(
        (m.topic, m.payload.decode(), m.retain))).start('#')
    base = 'phicomm/v2/u_70726573737572655f74657374/m1/aabbccddeefa'
    worker = None

    def wait(predicate, seconds=15):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            observer.check()
            if predicate():
                return
            assert worker.poll() is None, 'M1 worker exited'
            time.sleep(.05)
        raise AssertionError('pressure MQTT convergence timeout')

    def inject(key, values):
        worker.stdin.write(json.dumps({key: values}) + '\n')
        worker.stdin.flush()
        assert select.select([worker.stdout], [], [], 8)[0], key
        assert json.loads(worker.stdout.readline())['code'] == 'ok'

    def configs():
        return [(t, json.loads(p)) for t, p, r in records
                if t.endswith('m1_aabbccddeefa_pressure/config') and p]

    try:
        worker = subprocess.Popen([str(Path('build/simulator/M1').resolve()),
                                   'aabbccddeefa', '127.0.0.1', '127.0.0.1',
                                   str(port), 'pressure_test'],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        wait(lambda: any(t == base+'/sensor' and p for t, p, r in records))
        assert not configs(), 'No phantom pressure entity without a barometer'
        inject('sim_barometer', {'enabled': True, 'pressure_centi_pa': 10132567})
        wait(lambda: configs() and any(t == base+'/pressure' for t, p, r in records))
        topic, config = configs()[-1]
        assert config['device_class'] == 'atmospheric_pressure'
        assert config['unit_of_measurement'] == 'hPa'
        assert config['state_class'] == 'measurement' and config['expire_after'] == 10
        assert config['state_topic'] == base+'/pressure'
        assert config['availability_mode'] == 'all'
        assert {a['topic'] for a in config['availability']} == {
            base+'/availability', base+'/pressure_availability'}
        values = [json.loads(p) for t, p, r in records if t == base+'/pressure']
        assert values[-1] == {'pressure': 1013.2567}, values
        render = Environment(undefined=StrictUndefined).from_string(config['value_template'])
        assert float(render.render(value_json=values[-1])) == 1013.2567
        wait(lambda: any(t == base+'/pressure_availability' and p == 'online'
                         for t, p, r in records))
        start = len(records)
        observer.publish('homeassistant/status', 'online')
        wait(lambda: any(t == topic and p for t, p, r in records[start:]))
        replay = []
        late = Observer(port, lambda c, u, m: replay.append(m.retain))
        try:
            late.start(base+'/pressure')
            time.sleep(1.2)
            late.check()
            assert not any(replay), 'A previous sample must not replay as fresh'
        finally:
            late.close(failing=sys.exc_info()[0] is not None)
        # UART failure must not make the independent barometer unavailable.
        inject('sim_co2', {'enabled': True, 'ppm': 900})
        inject('sim_sensor', {'enabled': False})
        start = len(records)
        wait(lambda: any(t == base+'/sensor_availability' and p == 'offline'
                         for t, p, r in records[start:]), 22)
        assert any(t == base+'/pressure' for t, p, r in records[start:])
        assert not any(t == base+'/pressure_availability' and p == 'offline'
                       for t, p, r in records[start:])
        inject('sim_sensor', {'enabled': True})
        inject('sim_barometer', {'enabled': False})
        start = len(records)
        wait(lambda: any(t == base+'/pressure_availability' and p == 'offline'
                         for t, p, r in records[start:]), 15)
        assert any(t == base+'/sensor' and p for t, p, r in records[start:])
        assert any(t == base+'/co2' for t, p, r in records[start:])
        start = len(records)
        observer.publish('homeassistant/status', 'online')
        wait(lambda: any(t == topic and p for t, p, r in records[start:]))
        wait(lambda: any(t == base+'/pressure_availability' and p == 'offline'
                         for t, p, r in records[start:]))
        assert not any(t == base+'/pressure' for t, p, r in records[start:]), 'No stale sample after HA birth'
        inject('sim_barometer', {'enabled': True, 'pressure_centi_pa': 10001234})
        start = len(records)
        wait(lambda: any(t == base+'/pressure_availability' and p == 'online'
                         for t, p, r in records[start:]))
        wait(lambda: any(t == base+'/pressure' and json.loads(p)['pressure'] == 1000.1234
                         for t, p, r in records[start:]))
        print('PASS BMP388 MQTT: precision, HA template/birth, independent freshness, stale/recovery, non-retained samples')
    finally:
        cleanup(observer, [worker], failing=sys.exc_info()[0] is not None)


if __name__ == '__main__':
    main(int(sys.argv[1]))
