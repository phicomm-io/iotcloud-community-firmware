#!/usr/bin/env python3
"""Send exactly one non-retained, ESP32-only stock OTA command."""
import argparse,json,re,secrets,subprocess
from local_ota_server import normalize_device

def command(device,version):
    if not re.fullmatch(r'[A-Za-z0-9._-]{1,31}',version):raise ValueError('invalid target version')
    return 'SENSOR/'+normalize_device(device)+'/mqtt/service/ota',{'client_token':'iotcloud-'+secrets.token_hex(8),'data':{'upgrade_method':1,'upgrade_info':[{'device_type':'SENSOR3_ESP32','to_version':version}]}}
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--device',required=True);p.add_argument('--host',required=True);p.add_argument('--version',required=True);p.add_argument('--container',default='iotcloud-g3-migration');p.add_argument('--trigger-esp32',action='store_true',required=True);a=p.parse_args()
    topic,payload=command(a.device,a.version)
    subprocess.run(['docker','exec',a.container,'mosquitto_pub','-h',a.host,'-p','8883','--cafile','/lab/ca.pem','-q','1','-t',topic,'-m',json.dumps(payload,separators=(',',':'))],check=True,timeout=15)
    print('One ESP32-only command sent. Check HTTPS transfer and the rebooted device version.')
if __name__=='__main__':main()
