#!/usr/bin/env python3
"""Stage the pinned, independently released G3 MCU and font for online updates."""
import argparse,hashlib,json,os,re,shutil,urllib.request
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]

def assets(tag,directory,lock=None):
    lock=lock or json.loads((ROOT/'screen-release.lock.json').read_text())
    if lock['repository']!='phicomm-io/iotcloud-bry88bba15g-screenmcu' or not re.fullmatch('[0-9a-f]{40}',lock['commit']):raise ValueError('invalid screen source')
    if lock['release']!=tag:raise ValueError('screen release must match firmware release')
    result={'target':'BRY88BBA15G-screen','version':tag[1:],'source':{k:lock[k] for k in ('repository','commit','release')}}
    directory=Path(directory);directory.mkdir(parents=True,exist_ok=True)
    cache=Path(os.environ.get('XDG_CACHE_HOME',str(Path.home()/'.cache')))/'phicomm/screen-assets';cache.mkdir(parents=True,exist_ok=True)
    for kind,item in lock['assets'].items():
        if kind not in ('firmware','font','bootstrap') or not re.fullmatch('[A-Za-z0-9_.-]+',item['asset']) or not re.fullmatch('[0-9a-f]{64}',item['sha256']):raise ValueError('invalid screen asset')
        file=cache/item['sha256'];body=file.read_bytes() if file.exists() else b''
        if hashlib.sha256(body).hexdigest()!=item['sha256']:
            url=f"https://github.com/{lock['repository']}/releases/download/{tag}/{item['asset']}"
            with urllib.request.urlopen(url,timeout=90) as response:body=response.read(item['size']+1)
        if len(body)!=item['size'] or hashlib.sha256(body).hexdigest()!=item['sha256']:raise ValueError('screen asset digest mismatch')
        file.write_bytes(body)
        name={'firmware':'daikin_G3_screen.bin','font':'daikin_G3_zh32.iotfnt','bootstrap':'daikin_G3_recovery.bin'}[kind]
        (directory/name).write_bytes(body)
        result[kind]={'path':f'/releases/{tag}/{name}','size':len(body),'sha256':item['sha256']}
        if kind=='font':
            import zlib
            result[kind]['crc32']=zlib.crc32(body)
    import io,tarfile,gzip
    raw=io.BytesIO()
    with tarfile.open(fileobj=raw,mode='w') as archive:
        for file in sorted((ROOT/'tools/daikin_g3_migrate').iterdir()):
            if not file.is_file() or file.suffix not in ('.py','.swift','.sh','.md','.plist'):continue
            data=file.read_bytes();info=tarfile.TarInfo('daikin-g3-migrate/'+file.name);info.size=len(data);info.mode=0o755 if file.suffix in ('.py','.sh') else 0o644;archive.addfile(info,io.BytesIO(data))
        for name in ('flash_screen.py','backup_screen_boot.py'):
            data=(ROOT/'BRY88BBA15G/tools'/name).read_bytes()
            info=tarfile.TarInfo('daikin-g3-migrate/screen/'+name);info.size=len(data);info.mode=0o755;archive.addfile(info,io.BytesIO(data))
    packed=gzip.compress(raw.getvalue(),mtime=0)
    name='daikin_G3_migration.tar.gz'
    (directory/name).write_bytes(packed)
    result['migration']={'path':f'/releases/{tag}/{name}','size':len(packed),'sha256':hashlib.sha256(packed).hexdigest()}
    (directory/'screen-update.json').write_text(json.dumps(result,indent=2)+'\n')
    return result

def validate(screen,directory,tag):
    if screen.get('target')!='BRY88BBA15G-screen' or screen.get('version')!=tag[1:]:raise ValueError('incorrect screen target/version')
    for kind in ('firmware','font','bootstrap','migration'):
        item=screen[kind];name=Path(item['path']).name
        if item['path']!=f'/releases/{tag}/{name}':raise ValueError('screen asset path escapes release')
        body=(Path(directory)/name).read_bytes()
        if len(body)!=item['size'] or hashlib.sha256(body).hexdigest()!=item['sha256']:raise ValueError('screen asset digest mismatch')
        if kind in ('firmware','bootstrap') and len(body)!=421888:raise ValueError('incorrect MCU application length')
        if kind=='bootstrap' and b'install=factory-uart;mode=recovery;' not in body:raise ValueError('bootstrap requires recovery-only application')
        if kind=='font' and (not body.startswith(b'IOTFNT02') or len(body)>0x1fe000):raise ValueError('incorrect external font')
    return screen
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('tag');p.add_argument('output',type=Path);a=p.parse_args();assets(a.tag,a.output);validate(json.loads((a.output/'screen-update.json').read_text()),a.output,a.tag)
