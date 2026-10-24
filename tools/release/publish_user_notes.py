#!/usr/bin/env python3
"""Publish verified static note data to OTA mirrors, updating the pointer last."""
import argparse
from pathlib import Path
from urllib.request import ProxyHandler, Request, build_opener
from publish import get, put
from user_notes import read, valid_path, verify_output

MIRRORS = ('https://phicomm.fjrcn.com', 'https://phicomm.fjrcn.cn')


def publish(output):
    verify_output(output, formal=True)
    pointer_path = output / 'releases/notes/index.json'
    pointer = read(pointer_path)
    catalog = valid_path(output, pointer['catalog']).read_bytes()
    if get('releases/index.json') != catalog:
        raise ValueError('Mirror catalog differs from the verified page snapshot')
    manifest = pointer['manifest']
    body = valid_path(output, manifest).read_bytes()
    put(manifest['path'].lstrip('/'), body, True)
    # No public pointer refers to data until its immutable upload has completed.
    put('releases/notes/index.json', pointer_path.read_bytes())
    opener = build_opener(ProxyHandler({}))
    expected = {'/releases/notes/index.json': pointer_path.read_bytes(),
                manifest['path']: body, '/releases/index.json': catalog}
    for base in MIRRORS:
        for path, data in expected.items():
            request = Request(base + path + '?notes=' + manifest['sha256'],
                              headers={'Accept-Encoding': 'identity', 'Cache-Control': 'no-cache'})
            with opener.open(request, timeout=30) as response:
                if response.status != 200 or response.read() != data:
                    raise ValueError('Public note snapshot mismatch: ' + base + path)
    print('Verified static notes on both public mirrors:', manifest['sha256'])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    publish(parser.parse_args().output)
