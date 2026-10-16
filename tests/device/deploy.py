#!/usr/bin/env python3
"""Deploy one explicitly selected, signed candidate; verify identity/config/boot.

Run only with device-update authorization. Never retries an upload after a
transport failure: inspect device state and the saved evidence first.
"""
import argparse
import hashlib
import http.client
import json
import pathlib
import sys
import time
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / 'tools/ota'))
from package import verify_package
from client import Device,save

def validate_candidate(model, candidate, keyring_path=None):
    candidate=pathlib.Path(candidate)
    manifest=json.loads((candidate/'manifest.json').read_text())
    name=f'phicomm_{model}.ota.bin'
    raw=(candidate/name).read_bytes()
    image=(candidate/(name+'.signed')).read_bytes()
    if manifest.get('target')!=f'{model.upper()}@MK3080B@moc':
        raise ValueError('Candidate target does not match model')
    if hashlib.sha256(raw).hexdigest()!=manifest.get('files',{}).get(name):
        raise ValueError('Raw image does not match manifest file digest')
    image_meta=manifest.get('image',{})
    raw_digest=hashlib.sha256(raw).hexdigest()
    if image_meta.get('size')!=len(raw) or image_meta.get('sha256')!=raw_digest:
        raise ValueError('Raw image does not match manifest image metadata')
    signed=verify_package(image,model,keyring_path)
    if image[192:]!=raw or signed['payload_sha256']!=raw_digest:
        raise ValueError('Signed payload does not match raw image')
    if signed['version']!=manifest.get('base_version'):
        raise ValueError('Signed version does not match manifest base version')
    if signed['commit']!=manifest.get('git_commit'):
        raise ValueError('Signed commit does not match manifest commit')
    commit=manifest.get('git_commit')
    if not isinstance(commit,str) or len(commit)!=40 or any(c not in '0123456789abcdef' for c in commit):
        raise ValueError('Invalid manifest Git commit')
    if not isinstance(manifest.get('dirty'),bool):
        raise ValueError('Invalid manifest dirty state')
    expected_version=f"{manifest['base_version']}-{commit[:6]}"+('-dirty' if manifest['dirty'] else '')
    if manifest.get('version')!=expected_version:
        raise ValueError('Manifest runtime version is inconsistent')
    return manifest,image

def main():
    ap=argparse.ArgumentParser();ap.add_argument('--model',choices=['a1','m1'],required=True);ap.add_argument('--ip',required=True);ap.add_argument('--device-id',required=True);ap.add_argument('--candidate',type=pathlib.Path,required=True);ap.add_argument('--evidence',type=pathlib.Path,required=True);a=ap.parse_args()
    manifest,image=validate_candidate(a.model,a.candidate)
    d=Device(a.ip);before=d.get();config=d.get('config.get');assert before['device_id']==a.device_id
    save(a.evidence/'before.json',{'status':before,'config':config})
    connection=http.client.HTTPConnection(a.ip,timeout=60)
    connection.request('POST','/api/v1/ota/upload',image,{'Content-Type':'application/octet-stream'})
    response=connection.getresponse();result={'http_status':response.status,'body':response.read().decode(),'sha256':hashlib.sha256(image).hexdigest(),'manifest':manifest};connection.close();save(a.evidence/'upload.json',result)
    print(a.model,'verified upload response',result['http_status'],flush=True);assert result['http_status']==202
    time.sleep(3)
    def ready(s):
        return s['device_id']==a.device_id and s['version']==manifest['version'] and s['wifi_connected'] and s['uptime_ms']<before['uptime_ms'] and (s.get('panel_ready') if a.model=='a1' else s.get('sensor_valid'))
    after=d.wait(ready,seconds=120)
    after_config=d.get('config.get')
    for key in ('name','ssid','mqtt_host','mqtt_port','mqtt_user','wifi_password_set','mqtt_password_set','web_password_set'):
        assert config[key]==after_config[key],key
    save(a.evidence/'after.json',{'status':after,'config':after_config})
    print(a.model,'identity, restart, peripherals and preserved config verified',flush=True)
if __name__=='__main__':main()
