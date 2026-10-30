"""Render the offline product page using pinned Phicomm frontend assets."""
from pathlib import Path
import gzip
import hashlib
import json

WEB = Path(__file__).resolve().parent / 'web'

def render():
    text = (WEB / 'index.html').read_text()
    # Match the common renderer, which installs appearance before first paint.
    text = text.replace('</head>', '<script>/* @include appearance.js */</script></head>', 1)
    provenance = json.loads((WEB / 'shared/provenance.json').read_text())
    for name, expected in provenance['sha256'].items():
        data = (WEB / 'shared' / name).read_bytes()
        if hashlib.sha256(data).hexdigest() != expected:
            raise ValueError('Shared frontend asset changed: ' + name)
        text = text.replace('/* @include ' + name + ' */', data.decode())
    for name in ('page.js',):
        text = text.replace('/* @include ' + name + ' */', (WEB / name).read_text())
    if '/* @include ' in text:
        raise ValueError('Unresolved frontend include')
    return text.encode()

def header(html):
    data = gzip.compress(html, mtime=0)
    assert gzip.decompress(data) == html
    return '/* Generated offline Phicomm WebUI, gzip. */\nstatic const unsigned char ph_web[] = {\n' + '\n'.join(','.join(str(b) for b in data[i:i+24])+',' for i in range(0, len(data), 24)) + '\n};\n'
