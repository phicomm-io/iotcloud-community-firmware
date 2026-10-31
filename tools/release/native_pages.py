#!/usr/bin/env python3
"""Retain hash-verified native S30 downloads for every published release."""
import argparse
import hashlib
import json
from pathlib import Path
import re
from urllib.request import Request, urlopen

from release_version import parse_tag, version_key

PREFIX = 'https://github.com/phicomm-io/iotcloud-community-firmware/releases/download/'


def download(url):
    with urlopen(Request(url, headers={'User-Agent': 'iotcloud-pages'}), timeout=60) as response:
        return response.read()


def collect_release(tag, archive, fetch=download, source_commit=None):
    version = parse_tag(tag).version
    folder = archive / 'releases' / tag
    prefix = PREFIX + tag + '/'

    def read(name, expected=None, refresh=False):
        path = folder / name
        body = path.read_bytes() if path.is_file() else b''
        if refresh or not body or (expected and hashlib.sha256(body).hexdigest() != expected):
            body = fetch(prefix + name)
        if not body or (expected and hashlib.sha256(body).hexdigest() != expected):
            raise ValueError('Native release checksum mismatch: ' + name)
        folder.mkdir(parents=True, exist_ok=True)
        path.write_bytes(body)
        return body

    def checksums(refresh=False):
        values = {}
        for line in read('SHA256SUMS-S30', refresh=refresh).decode().splitlines():
            match = re.fullmatch(r'([0-9a-f]{64})  ([A-Za-z0-9_.-]+)', line)
            if not match or match[2] in values:
                raise ValueError('Invalid native release checksum list')
            values[match[2]] = match[1]
        return values

    sums = checksums()
    metadata = json.loads(read('build-s30.json', sums['build-s30.json']))
    # An explicitly reissued RC keeps its tag but has a different source.
    # Refresh retained assets only against that published catalog identity.
    if source_commit and metadata.get('project_head') != source_commit:
        sums = checksums(refresh=True)
        metadata = json.loads(read('build-s30.json', sums['build-s30.json']))
    validation = json.loads(read('validation-s30.json', sums['validation-s30.json']))
    identity = metadata['version_metadata']
    commit = metadata['project_head']
    if (not re.fullmatch('[0-9a-f]{40}', commit) or
            (source_commit is not None and source_commit != commit) or
            identity.get('git_commit') != commit or identity.get('base_version') != version or
            identity.get('git_tag') != tag or identity.get('dirty') is not False or
            metadata.get('dirty') is not False or
            metadata.get('version') != version + '-' + commit[:6] or
            identity.get('version') != metadata['version'] or
            metadata.get('flash_layout', {}).get('name') != 's30-v1b-stock-v1' or
            metadata['flash_layout'].get('bootloader_included') is not False or
            validation.get('status') != 'PASS'):
        raise ValueError('Invalid native release build identity or layout: ' + tag)
    downloads = {}
    for kind, name in [('rbl', f'BL2028N_OTA_{version}.rbl'),
                       ('stock_ua', f'BL2028N_STOCK_UA_{version}.bin')]:
        record = metadata['release_files'][name]
        if record['sha256'] != sums[name]:
            raise ValueError('Native build manifest and checksums differ: ' + name)
        body = read(name, sums[name])
        if len(body) != record['size']:
            raise ValueError('Native release size differs: ' + name)
        downloads[kind] = {'path': prefix + name, 'sha256': sums[name], 'size': len(body)}
    return {'tag': tag, 'source_commit': commit, 'models': {
        's30': {'checksums': prefix + 'SHA256SUMS-S30', 'downloads': downloads}}}


def collect(archive, fetch=download):
    tags = {}
    for name in ('index.json', 'candidates.json'):
        path = archive / 'releases' / name
        if path.is_file():
            for release in json.loads(path.read_bytes())['releases']:
                tag, commit = release['tag'], release.get('commit')
                if tag in tags and tags[tag] != commit:
                    raise ValueError('Conflicting native release catalog identity: ' + tag)
                tags[tag] = commit
    # Older GitHub candidates may have been withdrawn while their immutable
    # images remain in Pages. The reviewed source catalog pins those bytes.
    historical = json.loads((Path(__file__).resolve().parents[2] /
                             'site/native-downloads.json').read_bytes())
    known = {r['tag']: r for r in historical['releases']}
    releases = []
    for tag in sorted(tags, key=version_key, reverse=True):
        if version_key(tag) < version_key('v1.0.0-rc.4'):
            continue
        pinned = known.get(tag)
        if pinned and (not tags[tag] or pinned['source_commit'] == tags[tag]):
            for record in pinned['models']['s30']['downloads'].values():
                path = archive / 'releases' / tag / record['path'].rsplit('/', 1)[1]
                if (not path.is_file() or path.stat().st_size != record['size'] or
                        hashlib.sha256(path.read_bytes()).hexdigest() != record['sha256']):
                    break
            else:
                releases.append(pinned)
                continue
        releases.append(collect_release(tag, archive, fetch, tags[tag]))
    output = archive / 'releases/native-downloads.json'
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps({'schema': 1, 'releases': releases}, indent=2) + '\n')
    print(f'Archived native S30 downloads for {len(releases)} releases')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', type=Path)
    collect(parser.parse_args().archive)
