#!/usr/bin/env python3
"""Generate immutable release metadata, including downloads for every model."""
import argparse
import hashlib
import html
import json
import re
from pathlib import Path

from products import STEMS, asset_stem, variants_for
from release_version import parse_tag


def downloads(directory, tag, stem, legacy=False):
    files = {}
    for kind, suffix in [('signed', 'ota.bin.signed'), ('raw', 'ota.bin.raw'), ('full', 'all.bin')]:
        name = f'{stem}.{suffix}'
        if legacy and kind == 'raw' and not (directory / name).exists():
            name = f'{stem}.ota.bin'
        if legacy and kind == 'signed' and not (directory / name).exists():
            continue
        body = (directory / name).read_bytes()
        if not body:
            raise ValueError(f'empty asset {name}')
        files[kind] = dict(path=f'/releases/{tag}/{name}', size=len(body), sha256=hashlib.sha256(body).hexdigest())
    return files


def verify_daikin_assets(directory, files, tag, commit, variant, keyring_path):
    from verify import verify
    signed = (directory / Path(files['signed']['path']).name).read_bytes()
    # Authenticate the envelope before reading the signed fixed-offset label.
    verify(signed, 'daikin', tag, commit, variant, keyring_path)
    raw = (directory / Path(files['raw']['path']).name).read_bytes()
    if signed[192:] != raw:
        raise ValueError('DAIKIN raw image differs from authenticated signed payload')


def make(tag, commit, directory, notes, legacy=False, require_variants=False, keyring_path=None):
    identity = parse_tag(tag)
    if identity.prerelease and (legacy or identity.core[0] == 0):
        raise ValueError('Release candidates require the current v1+ signed asset layout')
    if not re.fullmatch('[0-9a-f]{40}', commit):
        raise ValueError('invalid commit')
    directory = Path(directory)
    release = dict(schema=1, tag=tag, version=tag[1:], commit=commit,
                   notes=f'/releases/{tag}/notes.md', models={})
    if identity.prerelease:
        release.update(prerelease=True, channel='candidate',
                       source={'scope':'candidate', 'kind':'relative-archive'},
                       github_release=f'https://github.com/phicomm-io/iotcloud-community-firmware/releases/tag/{tag}')
    if not legacy and any(directory.glob('phicomm_daikin.*')):
        raise ValueError('DAIKIN assets must use the daikin_BRY88AX151K prefix')
    include_slim = require_variants or any(directory.glob('*-no-frontend-resources.*'))
    for model, stem in STEMS.items():
        if model == 'daikin':
            if legacy and not any(directory.glob(stem + '.*')):
                stem = 'phicomm_daikin'
            if not require_variants and not any(directory.glob(stem + '.*')):
                continue
        files = downloads(directory, tag, stem, legacy)
        if model == 'daikin' and not tag.startswith('v0.'):
            verify_daikin_assets(directory, files, tag, commit, 'full', keyring_path)
        item = {**files.get('signed', files['raw']), 'downloads': files, 'release_log': release['notes']}
        if not tag.startswith('v0.') or (model != 'daikin' and include_slim):
            item['firmware_variant'] = 'full'
        release['models'][model] = item
        if legacy and 'signed' in files:
            signed = (directory / Path(files['signed']['path']).name).read_bytes()
            source = signed[96:116].hex()
            from verify import verify
            verify(signed, model, tag, source)
            if source != commit:
                import subprocess
                changed = subprocess.check_output(['git', 'diff', '--name-only', source, commit], text=True).splitlines()
                if any(not name.startswith('localdocs/') for name in changed):
                    raise ValueError('historical tag/source differ in build inputs')
                item.update(source_commit=source, source_relation='documentation_only_difference')
        for variant in variants_for(model, tag)[1:]:
            variant_stem = asset_stem(model, variant)
            required = require_variants or (variant == 'no-frontend-resources' and include_slim)
            if not required and not any(directory.glob(variant_stem + '.*')):
                continue
            files = downloads(directory, tag, variant_stem, legacy)
            if model == 'daikin' and not tag.startswith('v0.'):
                verify_daikin_assets(directory, files, tag, commit, variant, keyring_path)
            item.setdefault('variants', {})[variant] = {
                **files.get('signed', files['raw']), 'firmware_variant': variant,
                'downloads': files, 'release_log': release['notes']}
    (directory / 'notes.md').write_text(notes)
    (directory / 'index.html').write_text('<!doctype html><meta charset="utf-8"><title>' + tag +
                                         '</title><h1>' + tag + '</h1><pre>' + html.escape(notes) + '</pre>')
    if not legacy:
        from device_manifest import write_release
        write_release(release, directory)
    (directory / 'release.json').write_text(json.dumps(release, ensure_ascii=False, indent=2) + '\n')
    return release


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('tag')
    parser.add_argument('commit')
    parser.add_argument('directory', type=Path)
    parser.add_argument('notes', type=Path)
    args = parser.parse_args()
    make(args.tag, args.commit, args.directory, args.notes.read_text(), require_variants=True)
