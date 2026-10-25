#!/usr/bin/env python3
"""Offline verify the production envelope against the embedded keyring."""
import argparse
import importlib.util
from pathlib import Path
import struct

from products import asset_stem

spec = importlib.util.spec_from_file_location('release_ota_package', Path(__file__).resolve().parents[1] / 'ota/package.py')
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)


def verify_image(payload, model, variant):
    asset_stem(model, variant)
    if model == 'daikin':
        if (len(payload) < 352 or payload[0] != 0xe9 or
                struct.unpack_from('<I', payload, 32)[0] != 0xabcd5432 or
                payload[288:320] != b'BRY88AB151K'.ljust(32, b'\0') or
                payload[320:352] != ('PHICOMM-DAIKIN-1:' + variant).encode().ljust(32, b'\0')):
            raise ValueError('DAIKIN image hardware or firmware variant identity mismatch')


def verify(body, model, version, commit, variant=None, keyring_path=None):
    metadata = package.verify_package(body, model, keyring_path)
    if metadata['version'].removeprefix('v') != version.removeprefix('v') or metadata['commit'] != commit:
        raise ValueError('envelope version or commit mismatch')
    if variant is not None:
        verify_image(body[192:], model, variant)
    return metadata


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('file', type=Path)
    parser.add_argument('model', choices=['tc1', 'm1', 'a1', 'daikin'])
    parser.add_argument('version')
    parser.add_argument('commit')
    parser.add_argument('--variant', help='Also check the independently signed image variant identity')
    args = parser.parse_args()
    verify(args.file.read_bytes(), args.model, args.version, args.commit, args.variant)
