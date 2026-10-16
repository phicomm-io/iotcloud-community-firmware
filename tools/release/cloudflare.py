#!/usr/bin/env python3
"""Small Cloudflare API client; credentials are read only from the environment."""
import json, os, urllib.request, urllib.error
ACCOUNT=os.environ.get('CLOUDFLARE_ACCOUNT_ID','80aaa47452af8c1cbb774ac2a9a2f8e9')
ZONE=os.environ.get('CLOUDFLARE_ZONE_ID','8f668161af2aa7831c6304201c4f2637')
def api(path, method='GET', data=None, content_type='application/json', missing_ok=False):
    if data is not None and not isinstance(data,bytes):data=json.dumps(data).encode()
    req=urllib.request.Request('https://api.cloudflare.com/client/v4'+path,data=data,method=method,headers={'Authorization':'Bearer '+os.environ['CLOUDFLARE_API_TOKEN'],'Content-Type':content_type})
    try:
        with urllib.request.urlopen(req,timeout=90) as r:
            body=r.read()
            if content_type=='application/octet-stream' and method=='GET':return body
            value=json.loads(body)
    except urllib.error.HTTPError as e:
        if missing_ok and e.code==404:return None
        raise RuntimeError(f'Cloudflare {method} {path}: HTTP {e.code}: {e.read().decode()}') from None
    if not value.get('success'):raise RuntimeError(str(value.get('errors')))
    return value.get('result')
def usage():
    result=api(f'/accounts/{ACCOUNT}/r2/metrics')
    # Metrics use an account-wide total, not merely the new bucket.
    size=sum(v.get('payloadSize',0)+v.get('metadataSize',0) for tier in result.values() for v in tier.values())
    if size>10_000_000_000:raise RuntimeError('R2 exceeds 10 GB: delete old objects before uploading')
    return size
