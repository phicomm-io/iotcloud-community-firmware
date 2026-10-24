#!/usr/bin/env python3
"""Validate reviewed notes against staged signed assets before any publication."""
import argparse
import json
import shutil
import tempfile
from pathlib import Path
from archive_pages import write_latest
from user_notes import compile_manifest, read, version


def verify_staged(release_dir, retained):
    release = read(release_dir / 'release.json')
    catalog = read(retained / 'releases/index.json')
    existing = next((r for r in catalog['releases'] if r['tag'] == release['tag']), None)
    if existing and existing != release:
        raise ValueError('Published version metadata cannot be replaced')
    with tempfile.TemporaryDirectory(prefix='reviewed-release-') as directory:
        staged = Path(directory)
        shutil.copytree(retained / 'releases', staged / 'releases')
        shutil.copytree(release_dir, staged / 'releases' / release['tag'], dirs_exist_ok=True)
        catalog['releases'] = sorted([r for r in catalog['releases'] if r['tag'] != release['tag']] + [release], key=lambda r: version(r['version']), reverse=True)
        catalog['stable'] = release['tag']
        (staged / 'releases/index.json').write_text(json.dumps(catalog, ensure_ascii=False))
        write_latest(catalog, staged)
        manifest, _ = compile_manifest(archive=staged, formal=True)
        row = next(r for r in manifest['releases'] if r['tag'] == release['tag'])
        if row['history_status'] != 'reviewed' or not row['baseline_confirmed'] or any(
                c['status'] != 'complete' or set(c['locales']) != {'zh-CN', 'en'} for c in row['coverage']):
            raise ValueError('Staged release notes are not completely reviewed in both languages')
        print('Reviewed notes match all staged signed firmware targets:', release['tag'])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('release', type=Path)
    parser.add_argument('archive', type=Path)
    args = parser.parse_args()
    verify_staged(args.release, args.archive)
