"""Build the published screen simulator from a checksum-pinned source archive."""
import hashlib,io,json,os,re,tarfile,urllib.request
from pathlib import Path

def build(root,output):
    lock=json.loads((root/'screen-preview.lock.json').read_text())
    if lock['repository']!='phicomm-io/iotcloud-bry88bba15g-screenmcu' or not re.fullmatch('[0-9a-f]{40}',lock['commit']) or not re.fullmatch('[0-9a-f]{64}',lock['sha256']):raise ValueError('invalid screen preview source')
    cache=Path(os.environ.get('XDG_CACHE_HOME',str(Path.home()/'.cache')))/'phicomm/screen-preview';cache.mkdir(parents=True,exist_ok=True)
    file=cache/lock['sha256'];data=file.read_bytes() if file.exists() else b''
    if hashlib.sha256(data).hexdigest()!=lock['sha256']:
        url=f"https://codeload.github.com/{lock['repository']}/tar.gz/{lock['commit']}"
        with urllib.request.urlopen(url,timeout=90) as response:data=response.read(16000000)
    if hashlib.sha256(data).hexdigest()!=lock['sha256']:raise ValueError('screen preview archive digest mismatch')
    file.write_bytes(data);out=Path(output)/'dakin_lvgl';out.mkdir(parents=True,exist_ok=True)
    prefix='iotcloud-bry88bba15g-screenmcu-'+lock['commit']+'/simulator/ui-preview/'
    with tarfile.open(fileobj=io.BytesIO(data),mode='r:gz') as archive:
        for member in archive:
            if not member.isfile() or not member.name.startswith(prefix):continue
            path=Path(member.name[len(prefix):])
            if path.is_absolute() or '..' in path.parts or path.suffix not in ('.html','.js','.css','.svg','.json'):raise ValueError('unexpected preview asset')
            target=out/path;target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(archive.extractfile(member).read())
    if not (out/'index.html').is_file():raise ValueError('missing screen preview')
    (out/'source.json').write_text(json.dumps(lock,indent=2)+'\n')
