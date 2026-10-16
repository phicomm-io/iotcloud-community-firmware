#!/usr/bin/env python3
"""Embed the verified gzip bytes from the locked frontend release."""
from pathlib import Path
from frontend_release import fetch

ROOT = Path(__file__).resolve().parents[1]

def pack(model):
    if model not in ('tc1', 'm1', 'a1'):
        raise ValueError('Unknown frontend model')
    bundle = fetch(ROOT / 'frontend.lock.json')
    data = (bundle / 'devices' / model / 'index.html.gz').read_bytes()
    if model == 'tc1':
        output = ROOT / 'TC1/http_server/web_data.c'
        text = 'const unsigned char web_index_html[] = {' + ','.join(f'0x{b:02x}' for b in data) + '};\n'
    else:
        output = ROOT / model.upper() / 'generated/web_data.h'
        lines = ['#pragma once', f'static const unsigned char {model}_web_data[] = {{']
        for i in range(0, len(data), 16):
            lines.append('    ' + ','.join(f'0x{b:02x}' for b in data[i:i+16]) + ',')
        lines.append('};\n')
        text = '\n'.join(lines)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(text)
    print(f'{model.upper()} locked UI {len(data)} bytes gzip, frontend {bundle.name}')
