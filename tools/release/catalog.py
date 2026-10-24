#!/usr/bin/env python3
"""Generate immutable release metadata, including downloads for every model."""
import argparse, hashlib, html, json, re
from pathlib import Path
from products import STEMS, asset_stem
def make(tag, commit, directory, notes, legacy=False, require_variants=False):
    if not re.fullmatch(r'v(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)',tag):raise ValueError('invalid tag')
    if not re.fullmatch('[0-9a-f]{40}',commit):raise ValueError('invalid commit')
    release={'schema':1,'tag':tag,'version':tag[1:],'commit':commit,'notes':f'/releases/{tag}/notes.md','models':{}}
    if not legacy and any(directory.glob('phicomm_daikin.*')):
        raise ValueError('DAIKIN assets must use the daikin_BRY88AX151K prefix')
    for model,stem in STEMS.items():
        if model == 'daikin':
            if legacy and not any(directory.glob(stem + '.*')): stem = 'phicomm_daikin'
            if not require_variants and not any(directory.glob(stem + '.*')): continue
        files={}
        for kind,suffix in [('signed','ota.bin.signed'),('raw','ota.bin.raw'),('full','all.bin')]:
            name=f'{stem}.{suffix}'
            if legacy and kind=='raw' and not (directory/name).exists():name=f'{stem}.ota.bin'
            if legacy and kind=='signed' and not (directory/name).exists():continue
            body=(directory/name).read_bytes()
            if not body:raise ValueError(f'empty asset {name}')
            files[kind]={'path':f'/releases/{tag}/{name}','size':len(body),'sha256':hashlib.sha256(body).hexdigest()}
        release['models'][model]={**files.get('signed',files['raw']),'downloads':files,'release_log':release['notes']}
        if legacy and 'signed' in files:
            signed=(directory/Path(files['signed']['path']).name).read_bytes()
            source=signed[96:116].hex()
            from verify import verify
            verify(signed,model,tag,source)
            import subprocess
            if source!=commit:
                changed=subprocess.check_output(['git','diff','--name-only',source,commit],text=True).splitlines()
                if any(not name.startswith('localdocs/') for name in changed):raise ValueError('historical tag/source differ in build inputs')
                release['models'][model]['source_commit']=source
                release['models'][model]['source_relation']='documentation_only_difference'
    slim = 'no-frontend-resources'
    # Old catalogs remain readable. New production releases require all nine.
    include = require_variants or any(directory.glob('*-no-frontend-resources.*'))
    if include:
        for model in STEMS:
            if model=='daikin':continue
            files = {}
            for kind, suffix in [('signed','ota.bin.signed'),('raw','ota.bin.raw'),('full','all.bin')]:
                name = f'{asset_stem(model, slim)}.{suffix}'
                body = (directory / name).read_bytes()
                if not body: raise ValueError(f'empty asset {name}')
                files[kind] = dict(path=f'/releases/{tag}/{name}', size=len(body), sha256=hashlib.sha256(body).hexdigest())
            release['models'][model]['firmware_variant'] = 'full'
            release['models'][model]['variants'] = {slim: {**files['signed'], 'firmware_variant':slim, 'downloads':files, 'release_log':release['notes']}}
    for variant in ('fresh-air', 'fresh-air-no-frontend-resources'):
        if not require_variants and not any(directory.glob(f'phicomm_a1-{variant}.*')): continue
        files = {}
        for kind, suffix in [('signed','ota.bin.signed'),('raw','ota.bin.raw'),('full','all.bin')]:
            name = f'phicomm_a1-{variant}.{suffix}'
            body = (directory / name).read_bytes()
            if not body: raise ValueError(f'empty asset {name}')
            files[kind] = dict(path=f'/releases/{tag}/{name}',size=len(body),sha256=hashlib.sha256(body).hexdigest())
        release['models']['a1'].setdefault('variants', {})[variant] = {**files['signed'], 'firmware_variant':variant, 'downloads':files, 'release_log':release['notes']}
    (directory/'notes.md').write_text(notes)
    (directory/'index.html').write_text('<!doctype html><meta charset="utf-8"><title>'+tag+'</title><h1>'+tag+'</h1><pre>'+html.escape(notes)+'</pre>')
    (directory/'release.json').write_text(json.dumps(release,ensure_ascii=False,indent=2)+'\n')
    return release
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('tag');p.add_argument('commit');p.add_argument('directory',type=Path);p.add_argument('notes',type=Path);a=p.parse_args();make(a.tag,a.commit,a.directory,a.notes.read_text(),require_variants=True)
