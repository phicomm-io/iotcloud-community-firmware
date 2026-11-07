#!/usr/bin/env python3
"""Bounded stock gateway acknowledgements for one detector; OTA is a separate command."""
import argparse
import json
import os
import re
import subprocess
import time


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--device', required=True, help='12 hex STA MAC')
    p.add_argument('--gateway', required=True, help='12 hex virtual gateway MAC')
    p.add_argument('--host', required=True)
    p.add_argument('--container', default='iotcloud-g3-migration')
    p.add_argument('--cafile', default='/lab/ca.pem', help='Path inside container')
    p.add_argument('--seconds', type=int, default=180)
    p.add_argument('--response-delay', type=float, default=1.5,
                   help='Allow the MCU to consume +pub:OK before the application response')
    p.add_argument('--weather-code', type=int, choices=range(256),
                   help='Optional synthetic weather code for state-machine testing; not a weather observation')
    p.add_argument('--empty-gateway', action='store_true',
                   help='Explicit virtual gateway with no associated HVAC devices; replies with empty filter/error lists')
    a = p.parse_args()
    for value in (a.device, a.gateway):
        if not re.fullmatch(r'[0-9A-Fa-f]{12}', value):
            p.error('MAC must contain exactly 12 hex digits')
    if not 1 <= a.seconds <= 3600:
        p.error('--seconds must be 1..3600')
    if not 0 <= a.response_delay <= 5:
        p.error('--response-delay must be 0..5')
    root = f'SENSOR/{a.device.upper()}'
    routes = {
        f'{root}/mqtt/event/upload_snapshot': f'{root}/mqtt/event_response/snapshot_upload',
        f'{root}/mqtt/event/get_weather': f'{root}/mqtt/event_response/get_weather',
        f'{root}/IPBOX/{a.gateway.upper()}/mqtt/event/login_ipbox':
            f'{root}/IPBOX/{a.gateway.upper()}/mqtt/event_response/login_ipbox',
    }
    if a.empty_gateway:
        gateway_root = f'IPBOX/{a.gateway.upper()}/{root}/mqtt'
        for name in ('get_filter_sensor3', 'get_error_sensor3'):
            routes[f'{gateway_root}/service/{name}'] = f'{gateway_root}/service_response/{name}'
    base = ['docker', 'exec', a.container]
    conn = ['-h', a.host, '-p', '8883', '--cafile', a.cafile]
    client = f'iotcloud-compat-{os.getpid()}'
    cmd = base + ['mosquitto_sub'] + conn + ['-i', client, '-v', '-W', str(a.seconds)]
    for topic in routes:
        cmd += ['-t', topic]
    sub = subprocess.Popen(cmd, stdout=subprocess.PIPE, text=True)
    try:
        for line in sub.stdout:
            topic, separator, payload = line.rstrip('\n').partition(' ')
            if not separator or topic not in routes or len(payload) > 8192:
                continue
            try:
                request = json.loads(payload)
                token = request['client_token']
                if not isinstance(token, str) or not 1 <= len(token) <= 128:
                    continue
                data = request.get('data', {})
                if not isinstance(data, dict):
                    continue
                if topic.endswith('upload_snapshot'):
                    if data.get('physics', {}).get('mac') != a.device.upper():
                        continue
                elif data.get('terminal_mac', a.gateway.upper()) != a.gateway.upper():
                    continue
            except (ValueError, KeyError, TypeError, AttributeError):
                continue
            # Missing measurements use the factory sentinels. An explicit empty
            # virtual gateway has no HVAC filter/error entries, not a claim
            # about the detector's own sensors or calibration.
            response_data = {}
            if topic.endswith('/get_weather') and a.weather_code is not None:
                response_data = {'temperature': -100, 'humidity': 200,
                                 'pm25': -100, 'weatherCode': a.weather_code}
            elif a.empty_gateway and topic.endswith('/get_filter_sensor3'):
                response_data = {'filter_infos': []}
            elif a.empty_gateway and topic.endswith('/get_error_sensor3'):
                response_data = {'error_infos': []}
            body = json.dumps({'code': 0, 'data': response_data, 'client_token': token,
                               'timestamp': int(time.time() * 1000)}, separators=(',', ':'))
            time.sleep(a.response_delay)
            subprocess.run(base + ['mosquitto_pub'] + conn + ['-i', 'iotcloud-lan-compat-ack',
                           '-t', routes[topic], '-m', body], check=True, timeout=10)
            print(json.dumps({'event': 'compat-ack', 'topic': routes[topic],
                              'time': int(time.time())}), flush=True)
        rc = sub.wait()
        if rc not in (0, 27):  # mosquitto timeout is expected for this bounded run
            raise SystemExit(rc)
    finally:
        if sub.poll() is None:
            sub.terminate()
            sub.wait(timeout=5)


if __name__ == '__main__':
    main()
