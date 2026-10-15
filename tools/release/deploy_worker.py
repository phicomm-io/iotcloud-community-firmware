#!/usr/bin/env python3
"""Provision only phicomm OTA resources; preserve unrelated zone rules."""
import json, os, time
from pathlib import Path
from cloudflare import api, ACCOUNT, ZONE, usage
ROOT=Path(__file__).resolve().parents[2]
def main():
    backup=Path.home()/'.config/phicomm-ota'/('cloudflare-backup-'+str(int(time.time()))+'.json')
    backup.parent.mkdir(parents=True,exist_ok=True)
    records=api(f'/zones/{ZONE}/dns_records?name=phicomm.fjrcn.com')
    settings=api(f'/zones/{ZONE}/settings/always_use_https')
    rulesets=api(f'/zones/{ZONE}/rulesets')
    backup.write_text(json.dumps({'dns':records,'https':settings,'rulesets':rulesets},indent=2));backup.chmod(0o600)
    print('R2 account bytes:',usage())
    buckets=api(f'/accounts/{ACCOUNT}/r2/buckets')['buckets']
    if not any(b['name']=='phicomm-ota' for b in buckets):api(f'/accounts/{ACCOUNT}/r2/buckets','POST',{'name':'phicomm-ota'})
    rule={'ref':'phicomm_http_exception','description':'HTTPS everywhere except device OTA','expression':'(not ssl and http.host ne "phicomm.fjrcn.com")','action':'redirect','action_parameters':{'from_value':{'status_code':301,'target_url':{'expression':'concat("https://", http.host, http.request.uri.path)'},'preserve_query_string':True}},'enabled':True}
    phase='http_request_dynamic_redirect'
    entry=next((r for r in rulesets if r['phase']==phase and r['kind']=='zone'),None)
    if entry:
        existing=api(f'/zones/{ZONE}/rulesets/{entry["id"]}')
        current=next((r for r in existing['rules'] if r.get('ref')==rule['ref']),None)
        if current:
            api(f'/zones/{ZONE}/rulesets/{entry["id"]}/rules/{current["id"]}','PATCH',rule)
        else:
            api(f'/zones/{ZONE}/rulesets/{entry["id"]}/rules','POST',rule)
    else:api(f'/zones/{ZONE}/rulesets','POST',{'name':'Selective HTTPS','kind':'zone','phase':phase,'rules':[rule]})
    # The replacement redirect exists before removing the global redirect.
    api(f'/zones/{ZONE}/settings/always_use_https','PATCH',{'value':'off'})
    boundary='phicomm-worker-upload'
    metadata={'main_module':'src.js','compatibility_date':'2026-10-15','bindings':[{'name':'RELEASES','type':'r2_bucket','bucket_name':'phicomm-ota'}]}
    parts=[]
    for name,ctype,body in [('metadata','application/json',json.dumps(metadata)),('src.js','application/javascript+module',(ROOT/'infra/ota-worker/src.js').read_text())]:
        parts.append(f'--{boundary}\r\nContent-Disposition: form-data; name="{name}"; filename="{name}"\r\nContent-Type: {ctype}\r\n\r\n{body}\r\n')
    api(f'/accounts/{ACCOUNT}/workers/scripts/phicomm-ota','PUT',(''.join(parts)+f'--{boundary}--\r\n').encode(),'multipart/form-data; boundary='+boundary)
    domains=api(f'/accounts/{ACCOUNT}/workers/domains')
    if not any(d['hostname']=='phicomm.fjrcn.com' for d in domains):
        for record in records:
            if record['type']=='CNAME':api(f'/zones/{ZONE}/dns_records/{record["id"]}','DELETE')
        api(f'/accounts/{ACCOUNT}/workers/domains','PUT',{'hostname':'phicomm.fjrcn.com','zone_id':ZONE,'service':'phicomm-ota','environment':'production'})
    print('Worker deployed; rollback snapshot:',backup)
if __name__=='__main__':main()
