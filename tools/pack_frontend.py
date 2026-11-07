#!/usr/bin/env python3
"""Embed the verified frontend with the firmware HTTP queue adapter."""
from pathlib import Path
import hashlib
import gzip
import os
import json
import subprocess
import importlib.util
import re
import tinycss2
import zopfli.gzip
from frontend_release import fetch

ROOT = Path(__file__).resolve().parents[1]

def trim_screen_styles(css):
    """Native A1/M1/TC1 never mount the G3 screen editor's owned classes."""
    def trim(rules):
        kept = []
        for rule in rules:
            if rule.type == 'qualified-rule':
                groups = [[]]
                for token in rule.prelude:
                    if token.type == 'literal' and token.value == ',':
                        groups.append([])
                    else:
                        groups[-1].append(token)
                # Inspect only direct selector tokens. A class inside :not(),
                # :is(), :has() or an attribute cannot prove a rule impossible.
                groups = [group for group in groups if not any(
                    a.type == 'literal' and a.value == '.' and
                    b.type == 'ident' and b.value.startswith('screen-')
                    for a, b in zip(group, group[1:]))]
                if not groups:
                    continue
                rule.prelude = tinycss2.parse_component_value_list(','.join(tinycss2.serialize(group) for group in groups))
            elif rule.type == 'at-rule' and rule.lower_at_keyword in ('media', 'supports', 'container', 'layer') and rule.content is not None:
                rule.content = tinycss2.parse_component_value_list(tinycss2.serialize(trim(tinycss2.parse_rule_list(rule.content))))
            kept.append(rule)
        return kept
    return tinycss2.serialize(trim(tinycss2.parse_stylesheet(css)))


def adapt_http_queue(data, model):
    html = gzip.decompress(data)
    # The shared release also serves the G3 screen designer. Omit its CSS from
    # native products, keeping the locked UI logic, translations and behavior.
    text = html.decode('utf-8')
    text = re.sub(r'(<style\b[^>]*>)([\s\S]*?)(</style\s*>)',
                  lambda match: match[1] + trim_screen_styles(match[2]) + match[3], text, flags=re.I)
    html = text.encode('utf-8')
    marker = b'<head>'
    if html.count(marker) != 1:
        raise ValueError('Locked frontend has no unique head insertion point')
    # Serialize API requests through body completion. Keep explanatory comments
    # here: every byte in the injected adapter consumes device OTA capacity.
    if b'DeviceHttpQueue' not in html:
        adapter = (ROOT / 'common/web/http_queue.js').read_bytes()
        html = html.replace(marker, marker + b'<script>' + adapter + b'</script>', 1)
    # Compress the final bytes after injection, just like the frontend release.
    # Host-only: the device still serves a normal gzip stream verbatim.
    # M1's full UI is close to its fixed OTA slot. More host-side search keeps
    # the ordinary gzip stream and costs no device RAM or decoding work.
    output = zopfli.gzip.compress(html, numiterations=500 if model == 'm1' else 15)
    if gzip.decompress(output) != html:
        raise ValueError('Adapted frontend gzip round-trip mismatch')
    return output


def pack(model):
    if model not in ('tc1', 'm1', 'a1'):
        raise ValueError('Unknown frontend model')
    bundle = None
    local = os.environ.get('PHICOMM_FRONTEND_SOURCE')
    if not local: bundle = fetch(ROOT / 'frontend.lock.json')
    variant = os.environ.get('PHICOMM_FIRMWARE_VARIANT', 'full')
    allowed = ('full', 'no-frontend-resources') + (('fresh-air', 'fresh-air-no-frontend-resources') if model == 'a1' else ())
    if variant not in allowed: raise ValueError('Unknown variant')
    page = 'index.html.gz' if variant in ('full', 'fresh-air') else 'setup.html.gz'
    if local:
        if variant not in ('full','fresh-air'): raise ValueError('Development frontend requires full variant')
        source=Path(local).resolve()
        spec=importlib.util.spec_from_file_location('development_frontend',source/'common/web/page.py')
        renderer=importlib.util.module_from_spec(spec);spec.loader.exec_module(renderer)
        entry=source/('TC1/http_server/web/index.html' if model=='tc1' else model.upper()+'/web/index.html')
        raw=gzip.compress(renderer.render_page(entry,minify=True),mtime=0)
        provenance={'development':True,'source':str(source),
            'commit':subprocess.check_output(['git','-C',str(source),'rev-parse','HEAD'],text=True).strip(),
            'dirty':bool(subprocess.check_output(['git','-C',str(source),'status','--porcelain']))}
    else:
        raw=(bundle/'devices'/model/page).read_bytes()
        provenance=json.loads((ROOT/'frontend.lock.json').read_text())
    data=adapt_http_queue(raw,model)
    provenance['gzip_sha256']=hashlib.sha256(data).hexdigest()
    provenance['native_css_adapter']='omit-g3-screen-classes-v1'
    metadata=ROOT/'build'/('frontend-'+model+'-provenance.json')
    metadata.parent.mkdir(parents=True,exist_ok=True);metadata.write_text(json.dumps(provenance,indent=2)+'\n')

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
    print(f'{model.upper()} verified UI {len(data)} bytes gzip, frontend {bundle.name if bundle else "LOCAL DEVELOPMENT"}, HTTP FIFO=True')
