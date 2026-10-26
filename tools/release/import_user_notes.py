#!/usr/bin/env python3
"""Import immutable originals from a prepared archive; changed originals fail."""
import argparse
import json
from pathlib import Path
from user_notes import DATA, encoded, immutable, migrate, read, valid_path, version
from release_version import parse_tag


def import_archive(archive, data=DATA, previous=None, include_candidates=False):
    catalog = read(archive / 'releases/index.json')
    if include_candidates:
        candidates = read(archive / 'releases/candidates.json')
        if candidates.get('schema') != 1 or candidates.get('channel') != 'candidate' or 'stable' in candidates:
            raise ValueError('Invalid candidate archive catalog')
        for release in candidates['releases']:
            if not parse_tag(release['tag']).prerelease or release.get('channel') != 'candidate' or release.get('prerelease') is not True:
                raise ValueError('Invalid candidate archive release')
            existing = next((r for r in catalog['releases'] if r['tag'] == release['tag']), None)
            if existing is not None and existing != release:
                raise ValueError('Candidate archive metadata conflicts with a published release')
            if existing is None:
                catalog['releases'].append(release)
    order = read(data / 'release-order.json')
    known = {r['version'] for r in order['releases']}
    additions = sorted((r for r in catalog['releases'] if r['version'] not in known), key=lambda r: version(r['version']))
    if additions and (len(additions) != 1 or previous not in known):
        raise ValueError('Import one new release at a time with an explicit --previous archived version')
    for release in catalog['releases']:
        immutable(data / 'sources' / (release['tag'] + '.md'), valid_path(archive, dict(path=release['notes'])).read_bytes())
    for release in additions:
        order['releases'].append(dict(version=release['version'], previous=previous, branch='main', channel=parse_tag(release['tag']).channel))
    (data / 'release-order.json').write_bytes(encoded(order))
    (data / 'records.json').write_bytes(encoded(migrate(data)))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--archive', required=True, type=Path)
    parser.add_argument('--data', type=Path, default=DATA)
    parser.add_argument('--previous')
    parser.add_argument('--include-candidates', action='store_true', help='Explicitly import the separate candidate catalog as well as stable history')
    args = parser.parse_args()
    import_archive(args.archive, args.data, args.previous, args.include_candidates)
