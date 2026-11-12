#!/usr/bin/env python3
"""Explicitly enable one G3 ESP32 offer, preserving the existing broker certificate."""
import argparse,json,re,shutil
from pathlib import Path
from local_ota_server import load_app,normalize_device

def enable(directory,device,firmware,version):
    if not re.fullmatch(r'[A-Za-z0-9._-]{1,31}',version):raise ValueError('invalid target version')
    raw=load_app(firmware)
    compose=directory/'compose.json';data=json.loads(compose.read_text());args=data['services']['https']['command']
    if args[args.index('--device')+1]!=normalize_device(device):raise ValueError('device differs from prepared service')
    for key in ('--firmware','--version'):
        if key in args:i=args.index(key);del args[i:i+2]
    if '--enable-update' in args:args.remove('--enable-update')
    args+=['--enable-update','--firmware','/lab/esp32-app.bin','--version',version]
    tmp=directory/'esp32-app.bin.tmp';tmp.write_bytes(raw);tmp.replace(directory/'esp32-app.bin')
    tmp=directory/'compose.json.tmp';tmp.write_text(json.dumps(data,indent=2)+'\n');tmp.replace(compose)

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--directory',type=Path,required=True);p.add_argument('--device',required=True);p.add_argument('--firmware',type=Path,required=True);p.add_argument('--version',required=True);a=p.parse_args()
    try:enable(a.directory,a.device,a.firmware,a.version)
    except (ValueError,OSError,KeyError) as e:p.exit(2,str(e)+'\n')
    print('ESP32 offer enabled. Recreate only the HTTPS service; the broker certificate is unchanged.')
    print('Recommended: current RC6 G3 firmware and its matching migration bundle, or a newer compatible release. https://phicomm-io.github.io/#daikin-g3-downloads')
    print('建议使用当前 RC6 的配套 G3 固件和迁移工具包，或更新的兼容版本。')
if __name__=='__main__':main()
