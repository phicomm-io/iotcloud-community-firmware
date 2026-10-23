#!/usr/bin/env python3
"""Embed the verified frontend with the firmware HTTP queue adapter."""
from pathlib import Path
import hashlib
import gzip
import os
import zopfli.gzip
from frontend_release import fetch

ROOT = Path(__file__).resolve().parents[1]

def adapt_http_queue(data, model):
    html = gzip.decompress(data)
    marker = b'<head>'
    if html.count(marker) != 1:
        raise ValueError('Locked frontend has no unique head insertion point')
    # Serialize API requests through body completion. Keep explanatory comments
    # here: every byte in the injected adapter consumes device OTA capacity.
    adapter = (ROOT / 'common/web/http_queue.js').read_bytes()
    html = html.replace(marker, marker + b'<script>' + adapter + b'</script>', 1)
    # Compress the final bytes after injection, just like the frontend release.
    # Host-only: the device still serves a normal gzip stream verbatim.
    output = zopfli.gzip.compress(html, numiterations=15)
    if gzip.decompress(output) != html:
        raise ValueError('Adapted frontend gzip round-trip mismatch')
    return output


def pack(model):
    if model not in ('tc1', 'm1', 'a1'):
        raise ValueError('Unknown frontend model')
    bundle = fetch(ROOT / 'frontend.lock.json')
    variant = os.environ.get('PHICOMM_FIRMWARE_VARIANT', 'full')
    allowed = ('full', 'no-frontend-resources') + (('fresh-air', 'fresh-air-no-frontend-resources') if model == 'a1' else ())
    if variant not in allowed: raise ValueError('Unknown variant')
    page = 'index.html.gz' if variant in ('full', 'fresh-air') else 'setup.html.gz'
    data = adapt_http_queue((bundle / 'devices' / model / page).read_bytes(), model)
    generated = Path(os.environ.get('PHICOMM_WEB_OUTPUT_DIR', str(ROOT / model.upper() / 'generated')))
    if model == 'tc1':
        output = generated / 'web_data.c'
        text = 'const unsigned char web_index_html[] = {' + ','.join(f'0x{b:02x}' for b in data) + '};\n'
    else:
        output = generated / 'web_data.h'
        etag = hashlib.sha256(data).hexdigest()
        lines = ['#pragma once', f'#define {model.upper()}_WEB_ETAG "\\\"{etag}\\\""',
                 f'static const unsigned char {model}_web_data[] = {{']
        for i in range(0, len(data), 16):
            lines.append('    ' + ','.join(f'0x{b:02x}' for b in data[i:i+16]) + ',')
        lines.append('};\n')
        text = '\n'.join(lines)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(text)
    print(f'{model.upper()} verified UI {len(data)} bytes gzip, frontend {bundle.name}, HTTP FIFO=True')
