#!/usr/bin/env python3
"""Import immutable originals from a prepared archive; changed originals fail."""
import argparse
import json
from pathlib import Path
from user_notes import DATA, encoded, immutable, migrate, read, valid_path, version


def import_archive(archive, data=DATA, previous=None):
    catalog = read(archive / 'releases/index.json')
    order = read(data / 'release-order.json')
    known = {r['version'] for r in order['releases']}
    additions = sorted((r for r in catalog['releases'] if r['version'] not in known), key=lambda r: version(r['version']))
    if additions and (len(additions) != 1 or previous not in known):
        raise ValueError('Import one new release at a time with an explicit --previous archived version')
    for release in catalog['releases']:
        immutable(data / 'sources' / (release['tag'] + '.md'), valid_path(archive, dict(path=release['notes'])).read_bytes())
    for release in additions:
        order['releases'].append(dict(version=release['version'], previous=previous, branch='main', channel='stable'))
    (data / 'release-order.json').write_bytes(encoded(order))
    (data / 'records.json').write_bytes(encoded(migrate(data)))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--archive', required=True, type=Path)
    parser.add_argument('--data', type=Path, default=DATA)
    parser.add_argument('--previous')
    args = parser.parse_args()
    import_archive(args.archive, args.data, args.previous)
