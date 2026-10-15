#!/usr/bin/env python3
"""Generate immutable release metadata, including downloads for every model."""
import argparse, hashlib, html, json, re
from pathlib import Path
PRODUCTS={'tc1':'tc1_a1','m1':'m1','a1':'a1'}
def make(tag, commit, directory, notes, legacy=False):
    if not re.fullmatch(r'v(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)',tag):raise ValueError('invalid tag')
    if not re.fullmatch('[0-9a-f]{40}',commit):raise ValueError('invalid commit')
    release={'schema':1,'tag':tag,'version':tag[1:],'commit':commit,'notes':f'/releases/{tag}/notes.md','models':{}}
    for model,product in PRODUCTS.items():
        files={}
        for kind,suffix in [('signed','ota.bin.signed'),('raw','ota.bin.raw'),('full','all.bin')]:
            name=f'phicomm_{product}.{suffix}'
            if legacy and kind=='raw' and not (directory/name).exists():name=f'phicomm_{product}.ota.bin'
            if legacy and kind=='signed' and not (directory/name).exists():continue
            body=(directory/name).read_bytes()
            if not body:raise ValueError(f'empty asset {name}')
            files[kind]={'path':f'/releases/{tag}/{name}','size':len(body),'sha256':hashlib.sha256(body).hexdigest()}
        release['models'][model]={**files.get('signed',files['raw']),'downloads':files,'release_log':release['notes']}
    (directory/'notes.md').write_text(notes)
    (directory/'index.html').write_text('<!doctype html><meta charset="utf-8"><title>'+tag+'</title><h1>'+tag+'</h1><pre>'+html.escape(notes)+'</pre>')
    (directory/'release.json').write_text(json.dumps(release,ensure_ascii=False,indent=2)+'\n')
    return release
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('tag');p.add_argument('commit');p.add_argument('directory',type=Path);p.add_argument('notes',type=Path);a=p.parse_args();make(a.tag,a.commit,a.directory,a.notes.read_text())
