#!/usr/bin/env python3
"""Prepare an isolated LAN compatibility service, initially offering no update."""
import argparse,json,os,re,shutil,sys
from pathlib import Path
from local_ota_server import create_certificate,load_app,normalize_device

def prepare(a):
    a.device=normalize_device(a.device)
    if a.version and not re.fullmatch(r'[A-Za-z0-9_.-]{1,31}',a.version):raise ValueError('invalid target version')
    if a.enable_update and (not a.firmware or not a.version):raise ValueError('updates require firmware and version')
    if not a.enable_update and (a.firmware or a.version):raise ValueError('use --enable-update explicitly')
    a.output.mkdir(parents=True,exist_ok=True)
    if (a.output/'compose.json').exists():raise ValueError('choose a new output directory')
    if a.enable_update:load_app(a.firmware)
    a.cert=a.output/'ca.pem';a.key=a.output/'server.key';create_certificate(a)
    shutil.copyfile(a.cert,a.output/'server.pem')
    a.output.chmod(0o700)
    (a.output/'mosquitto.conf').write_text(('user root\n' if os.getuid()==0 else '')+'listener 8883 0.0.0.0\nallow_anonymous true\ncertfile /lab/server.pem\nkeyfile /lab/server.key\nrequire_certificate false\npersistence false\nlog_type error\nlog_type warning\n')
    source=Path(__file__).resolve().parent
    for name in ['local_ota_server.py','local_gateway_responder.py','trigger.py']:shutil.copyfile(source/name,a.output/name)
    args=['python3','/lab/local_ota_server.py','serve','--bind','0.0.0.0','--port','8443','--device',a.device,'--origin','https://'+a.address,'--cert','/lab/ca.pem','--key','/lab/server.key','--broker-ca','/lab/ca.pem']
    if a.enable_update:
        shutil.copyfile(a.firmware,a.output/'esp32-app.bin');args+=['--enable-update','--firmware','/lab/esp32-app.bin','--version',a.version]
    services={'mqtt':{'user':str(os.getuid())+':'+str(os.getgid()),'image':'eclipse-mosquitto:2.0.22','container_name':'iotcloud-g3-migration','ports':[a.address+':8883:8883'],'volumes':[str(a.output.resolve())+':/lab:ro'],'command':['mosquitto','-c','/lab/mosquitto.conf']},'https':{'image':'python:3.12-slim','ports':[a.address+':443:8443'],'volumes':[str(a.output.resolve())+':/lab:ro'],'command':args}}
    (a.output/'compose.json').write_text(json.dumps({'services':services},indent=2)+'\n')
    print('Prepared',a.output,'mode:', 'ESP32 update enabled' if a.enable_update else 'no update')
    print('Use the current RC6 migration bundle and matching G3 firmware, or a newer compatible release: https://phicomm-io.github.io/#daikin-g3-downloads')
    print('请使用当前 RC6 的完整迁移工具包与配套 G3 固件，或更新的兼容版本。')

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--address',required=True);p.add_argument('--device',required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--enable-update',action='store_true');p.add_argument('--firmware',type=Path);p.add_argument('--version');a=p.parse_args()
    try:prepare(a)
    except (ValueError,OSError) as e:p.exit(2,str(e)+'\n')
if __name__=='__main__':main()
