#!/usr/bin/env python3
"""Embed the verified gzip bytes from the locked frontend release."""
from pathlib import Path
import hashlib
import os
from frontend_release import fetch

ROOT = Path(__file__).resolve().parents[1]

def pack(model):
    if model not in ('tc1', 'm1', 'a1'):
        raise ValueError('Unknown frontend model')
    bundle = fetch(ROOT / 'frontend.lock.json')
    variant = os.environ.get('PHICOMM_FIRMWARE_VARIANT', 'full')
    if variant not in ('full', 'no-frontend-resources'): raise ValueError('Unknown variant')
    page = 'index.html.gz' if variant == 'full' else 'setup.html.gz'
    data = (bundle / 'devices' / model / page).read_bytes()
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
    print(f'{model.upper()} locked UI {len(data)} bytes gzip, frontend {bundle.name}')
