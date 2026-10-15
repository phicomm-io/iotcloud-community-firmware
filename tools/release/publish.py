#!/usr/bin/env python3
"""Upload immutable assets, verify both public mirrors, then promote atomically.
Run only under the workflow's production concurrency group. Never overwrite
an immutable release with different bytes, even on retry.
"""
import argparse, hashlib, json, subprocess, urllib.parse, urllib.request
from pathlib import Path
from cloudflare import api, ACCOUNT, usage
from verify import verify as verify_signature
BUCKET=f'/accounts/{ACCOUNT}/r2/buckets/phicomm-ota/objects/'
def get(key):
    return api(BUCKET+urllib.parse.quote(key,safe=''),content_type='application/octet-stream',missing_ok=True)
def put(key,body,immutable=False):
    if immutable:
        old=get(key)
        if old is not None:
            if old!=body:raise ValueError('immutable object conflict: '+key)
            return
    api(BUCKET+urllib.parse.quote(key,safe=''),'PUT',body,'application/octet-stream')
def verify(release):
    for base in ['http://phicomm.fjrcn.com','http://phicomm.fjrcn.cn']:
        for model_name,model in release['models'].items():
            for kind,asset in model['downloads'].items():
                req=urllib.request.Request(base+asset['path'],headers={'Accept-Encoding':'identity','User-Agent':''})
                with urllib.request.build_opener(urllib.request.ProxyHandler({})).open(req,timeout=120) as r:
                    if r.url!=req.full_url or r.status!=200 or r.headers.get('Transfer-Encoding') or r.headers.get('Content-Encoding') or int(r.headers['Content-Length'])!=asset['size']:raise ValueError('not a direct fixed-length download')
                    body=r.read()
                if hashlib.sha256(body).hexdigest()!=asset['sha256']:raise ValueError('mirror hash mismatch')
                if kind=='signed':verify_signature(body,model_name,release['version'],release['commit'])
def smoke(release):
    opener=urllib.request.build_opener(urllib.request.ProxyHandler({}))
    for base in ['http://phicomm.fjrcn.com','http://phicomm.fjrcn.cn']:
        for model in release['models']:
            url=f"{base}/ota/{model}/latest.json?release={release['tag']}"
            with opener.open(urllib.request.Request(url,headers={'User-Agent':'','Accept-Encoding':'identity'}),timeout=30) as response:
                latest=json.load(response)
            if latest['tag']!=release['tag'] or latest['model']!=model or latest['sha256']!=release['models'][model]['sha256']:
                raise ValueError('latest manifest smoke test failed')

def version(tag):return tuple(map(int,tag.removeprefix('v').split('.')))
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('directory',type=Path);p.add_argument('--promote',action='store_true');p.add_argument('--archive-only',action='store_true');a=p.parse_args()
    release=json.loads((a.directory/'release.json').read_text());tag=release['tag'];print('R2 account bytes:',usage())
    stable=get('stable.json')
    if not a.archive_only and stable and version(json.loads(stable)['tag'])>version(tag):raise ValueError('refuse to publish a version older than stable')
    if not a.promote:
        for f in sorted(a.directory.iterdir()):
            if f.is_file() and not f.name.startswith('SHA256SUMS-'):put(f'releases/{tag}/{f.name}',f.read_bytes(),True)
        verify(release)
        if not a.archive_only:return
    index_raw=get('releases/index.json');index=json.loads(index_raw) if index_raw else {'schema':1,'releases':[]}
    entries=[r for r in index['releases'] if r['tag']!=tag]+[release];index['releases']=sorted(entries,key=lambda r:version(r['tag']),reverse=True)
    # Only public, complete releases enter this catalog. A single pointer carries
    # all latest manifests; no window with different model versions.
    if a.promote:
        verify(release)
        previous=get('stable.json')
        if previous and version(json.loads(previous)['tag'])>version(tag):raise ValueError('refuse to downgrade stable')
        if previous:put('rollback/stable.json',previous)
        put('stable.json',json.dumps(release).encode())
        try:smoke(release)
        except Exception:
            if previous:put('stable.json',previous)
            else:api(BUCKET+urllib.parse.quote('stable.json',safe=''),'DELETE')
            raise
        index['stable']=tag
    put('releases/index.json',json.dumps(index,ensure_ascii=False).encode())
    print('Published catalog:',tag)
if __name__=='__main__':main()
