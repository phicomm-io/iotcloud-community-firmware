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
        for model_name,original in release['models'].items():
          for variant,model in [('full', original), *original.get('variants', {}).items()]:
            for kind,asset in model['downloads'].items():
                req=urllib.request.Request(base+asset['path'],headers={'Accept-Encoding':'identity','User-Agent':''})
                with urllib.request.build_opener(urllib.request.ProxyHandler({})).open(req,timeout=120) as r:
                    if r.url!=req.full_url or r.status!=200 or r.headers.get('Transfer-Encoding') or r.headers.get('Content-Encoding') or int(r.headers['Content-Length'])!=asset['size']:raise ValueError('not a direct fixed-length download')
                    body=r.read()
                if hashlib.sha256(body).hexdigest()!=asset['sha256']:raise ValueError('mirror hash mismatch')
                if kind=='signed':verify_signature(body,model_name,release['version'],model.get('source_commit',release['commit']),
                                                  variant=variant if version(release['tag']) >= (1,0,0) else None)
def smoke(release, bridge=None):
    opener=urllib.request.build_opener(urllib.request.ProxyHandler({}))
    for base in ['http://phicomm.fjrcn.com','http://phicomm.fjrcn.cn']:
      channels = [(release, version(release['tag']) >= (1,0,0))]
      if version(release['tag']) >= (1,0,0):
          bridge = release if version(release['tag']) == (1,0,0) else bridge
          if not bridge or bridge.get('tag') != 'v1.0.0':raise ValueError('bridge release is required for legacy manifest smoke test')
          channels.append((bridge, False))
      for selected, current in channels:
        for model, original in selected['models'].items():
          for variant, entry in [('full',original), *original.get('variants',{}).items()]:
            if not current and variant == 'hub':continue
            suffix = '' if variant == 'full' else '/' + variant
            path=f"/ota/v1/{model}/{variant}" if current else f"/ota/{model}{suffix}"
            url=f"{base}{path}/latest.json?release={selected['tag']}"
            with opener.open(urllib.request.Request(url,headers={'User-Agent':'','Accept-Encoding':'identity'}),timeout=30) as response:
                latest=json.load(response)
            if latest['tag']!=selected['tag'] or latest['model']!=model or latest['sha256']!=entry['sha256'] or latest.get('firmware_variant','full')!=variant:
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
        bridge_release = None
        if version(tag) == (1,0,0):
            put('bridge-v1.0.0.json',json.dumps(release).encode(),True)
        elif version(tag) > (1,0,0):
            bridge=get('bridge-v1.0.0.json')
            bridge_release = json.loads(bridge) if bridge else None
            if not bridge_release or bridge_release.get('tag')!='v1.0.0' or bridge_release.get('version')!='1.0.0':raise ValueError('publish v1.0.0 bridge before later stable versions')
        put('stable.json',json.dumps(release).encode())
        try:smoke(release, bridge_release)
        except Exception:
            if previous:put('stable.json',previous)
            else:api(BUCKET+urllib.parse.quote('stable.json',safe=''),'DELETE')
            raise
        index['stable']=tag
    put('releases/index.json',json.dumps(index,ensure_ascii=False).encode())
    print('Published catalog:',tag)
if __name__=='__main__':main()
