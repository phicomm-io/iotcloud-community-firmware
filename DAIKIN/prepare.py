"""Embed the exact DAIKIN page from the pinned shared frontend release."""
Import('env')
import gzip
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
root = Path(env['PROJECT_DIR']).parent
sys.path.insert(0, str(root / 'tools'))
from frontend_release import fetch
from check_version import resolve_version
local = os.environ.get('PHICOMM_FRONTEND_SOURCE')
screen = env['PIOENV'] == 'bry88bba15g'
variant = 'hub' if env['PIOENV'] == 'bry88ab-hub' else 'full'

def compress_text(content):
    import zopfli.gzip
    compressed = zopfli.gzip.compress(content, numiterations=500)
    if gzip.decompress(compressed) != content:
        raise RuntimeError('Hub frontend gzip round-trip mismatch')
    return compressed

if local:
    source = Path(local).resolve()
    if variant == 'full':
        spec = importlib.util.spec_from_file_location('development_frontend', source/'common/web/page.py')
        renderer = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(renderer)
        data = gzip.compress(renderer.render_page(source/'DAIKIN/web/index.html', minify=True, screen=screen), mtime=0)
    frontend = {'development': True,
                'commit': subprocess.check_output(['git','-C',str(source),'rev-parse','HEAD'], text=True).strip(),
                'dirty': bool(subprocess.check_output(['git','-C',str(source),'status','--porcelain'], text=True).strip())}
else:
    bundle = fetch(root/'frontend.lock.json')
    if variant == 'full':
        data = (bundle/'devices'/('daikin-g3' if screen else 'daikin')/'index.html.gz').read_bytes()
    frontend = json.loads((root/'frontend.lock.json').read_text())
if variant == 'hub':
    # A development build may live in the shared cache instead of frontend/dist.
    # Verify its product mode before embedding an otherwise valid Cloud bundle.
    override = os.environ.get('PHICOMM_HUB_FRONTEND_DIST')
    if override and not local:
        raise RuntimeError('PHICOMM_HUB_FRONTEND_DIST requires PHICOMM_FRONTEND_SOURCE')
    hub_root = Path(override).resolve() if override else source/'frontend/dist' if local else bundle/'hub'
    if local:
        marker = hub_root/'frontend-build.json'
        try:
            build = json.loads(marker.read_text())
        except (OSError, ValueError) as error:
            raise RuntimeError('Hub frontend build metadata missing; build with VITE_HUB=1') from error
        if not isinstance(build, dict) or build.get('schema') != 1 or build.get('mode') != 'hub':
            raise RuntimeError('Hub frontend requires a VITE_HUB=1 build, not a Cloud bundle')
        frontend['hub_build'] = build
    standalone = hub_root/'standalone-daikin.html'
    if not (hub_root/'index.html').is_file() or not standalone.is_file():
        raise RuntimeError('Hub shared frontend missing; build the shared frontend with VITE_HUB=1')
    data = compress_text(standalone.read_bytes())
if local:
    inputs = [p for folder in ('common/web', 'DAIKIN/web') for p in (source/folder).rglob('*') if p.is_file() and p.suffix in ('.js','.css','.html','.py')]
    frontend['source_sha256'] = {p.relative_to(source).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(inputs)}
    patch = subprocess.check_output(['git','-C',str(source),'diff','--binary','HEAD','--','common/web','DAIKIN/web'])
    frontend['tracked_patch_sha256'] = hashlib.sha256(patch).hexdigest()
frontend['gzip_sha256'] = hashlib.sha256(data).hexdigest()
out = root/'DAIKIN/generated'/env['PIOENV']
out.mkdir(parents=True, exist_ok=True)
(out/'web.h').write_text('#pragma once\nstatic const unsigned char web_data[] = {' + ','.join(map(str,data)) + '};\n')
(out/'frontend.json').write_text(json.dumps(frontend, indent=2)+'\n')
env.Append(CPPPATH=[str(out)])
metadata = resolve_version(root)
if screen:
    # Specialize the independently exported G3 frontend's product label.
    page = gzip.decompress(data).replace(b'BRY88AX151K', b'BRY88BBA15G').replace(b'BRY88AB151K', b'BRY88BBA15G').replace(b'DAIKIN G1/G2', b'DAIKIN G3')
    # This board has no discrete air-quality lamps or direct CM1106 controls.
    # Keep the shared page/layout, hide only the unsupported product controls.
    board_style = b'<style>section.card:has(#indicator),#indicatorSettings,#climateSettings,#co2Settings{display:none!important}</style>'
    page = page.replace(b'</head>', board_style + b'</head>')
    handoff = (root/'BRY88BBA15G'/'wifi-handoff.js').read_bytes()
    if page.count(b'</body>') != 1:
        raise RuntimeError('Missing G3 frontend script insertion point')
    page = page.replace(b'</body>', b'<script>' + handoff + b'</script></body>')
    data = gzip.compress(page, mtime=0)
    (out/'web.h').write_text('#pragma once\nstatic const unsigned char web_data[] = {' + ','.join(map(str,data)) + '};\n')
    frontend['product'] = 'BRY88BBA15G'
    frontend['wifi_handoff_sha256'] = hashlib.sha256(handoff).hexdigest()
    frontend['adapted_gzip_sha256'] = hashlib.sha256(data).hexdigest()
    (out/'frontend.json').write_text(json.dumps(frontend, indent=2)+'\n')
# Board ABI used by the already installed firmware. The display name is AX.
(out/'build_info.h').write_text('#pragma once\n#define DAIKIN_VERSION ' + json.dumps(metadata['version']) + '\n#define DAIKIN_VARIANT ' + json.dumps(variant) + '\n#define DAIKIN_TARGET ' + json.dumps('BRY88BBA15G' if screen else 'BRY88AB151K') + '\n')
(out/(env['PIOENV']+'-version.json')).write_text(json.dumps(metadata, indent=2)+'\n')
if screen:
    with (out/'build_info.h').open('a') as file:
        file.write('#define BRY_UART_VERSION '+json.dumps('C-'+metadata['git_commit'][:6])+'\n')
env.Append(LINKFLAGS=['-Wl,-u,daikin_target'])

if variant == 'hub':
    # The development override is deliberately visible in package provenance.
    assets = []
    declarations = ['#pragma once', '#include <stddef.h>']
    for path in sorted(p for p in hub_root.rglob('*') if p.is_file()):
        relative = path.relative_to(hub_root).as_posix()
        if relative.endswith('.map') or relative in ('sw.js', 'config.json', 'standalone-daikin.html'):
            continue
        content = path.read_bytes()
        # Images are already compressed; retain their exact bytes in an ordinary
        # gzip wrapper. All textual resources use deterministic host-only Zopfli.
        suffix = path.suffix
        compressed = compress_text(content) if suffix in ('.html','.js','.css','.json','.webmanifest','.svg') else gzip.compress(content, compresslevel=9, mtime=0)
        if gzip.decompress(compressed) != content:
            raise RuntimeError('Hub frontend gzip round-trip mismatch: ' + relative)
        name = 'hub_asset_' + str(len(assets))
        declarations.append('static const unsigned char '+name+'[] = {'+','.join(map(str,compressed))+'};')
        mime = {'.html':'text/html; charset=utf-8','.js':'application/javascript','.css':'text/css','.json':'application/json','.webmanifest':'application/manifest+json','.svg':'image/svg+xml','.png':'image/png','.ico':'image/x-icon'}.get(suffix,'application/octet-stream')
        assets.append('{' + json.dumps('/iotcloud/'+relative) + ',' + json.dumps(mime) + ',' + name + ',sizeof('+name+')}')
    declarations.append('struct HubWebAsset {const char *path,*type;const unsigned char *data;size_t size;};')
    declarations.append('static const HubWebAsset hub_assets[] = {'+','.join(assets)+'};')
    (out/'hub_web.h').write_text('\n'.join(declarations)+'\n')
    frontend['hub_files'] = {str(p.relative_to(hub_root)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(hub_root.rglob('*')) if p.is_file()}
    (out/'frontend.json').write_text(json.dumps(frontend,indent=2)+'\n')
