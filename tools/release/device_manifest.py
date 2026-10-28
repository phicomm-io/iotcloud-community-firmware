"""Canonical OTA documents and dependency-free MessagePack encoding.

This encoder deliberately supports only the flat, typed device manifest, not
arbitrary application messages. JSON and MP are encoded from the same mapping.
"""
import hashlib
import json
import struct
from pathlib import Path
from products import asset_stem


def pack(value):
    if isinstance(value, str):
        data = value.encode('utf-8')
        n = len(data)
        if n < 32: return bytes([0xa0 | n]) + data
        if n < 256: return b'\xd9' + bytes([n]) + data
        if n < 65536: return b'\xda' + struct.pack('>H', n) + data
    elif type(value) is int and 0 <= value <= 0xffffffff:
        if value < 128: return bytes([value])
        if value < 256: return b'\xcc' + bytes([value])
        if value < 65536: return b'\xcd' + struct.pack('>H', value)
        return b'\xce' + struct.pack('>I', value)
    elif isinstance(value, dict) and len(value) < 16:
        return bytes([0x80 | len(value)]) + b''.join(pack(k) + pack(v) for k, v in value.items())
    raise ValueError('unsupported OTA manifest value')


def document(release, model, variant, entry):
    return dict(schema=1, tag=release['tag'], version=release['version'],
                commit=release['commit'], model=model, firmware_variant=variant,
                **entry['downloads']['signed'], release_log=release['notes'])


def encoded(document):
    values = {'json': (json.dumps(document, ensure_ascii=False, separators=(',', ':')) + '\n').encode(),
              'msgpack': pack(document)}
    if any(len(data) > 4096 for data in values.values()):
        raise ValueError('Device manifest exceeds 4096 bytes')
    return values


def write_release(release, directory):
    manifests = {}
    for model, original in release['models'].items():
        for variant, entry in [('full', original), *original.get('variants', {}).items()]:
            if 'signed' not in entry['downloads']: continue
            pair = {}
            for format, data in encoded(document(release, model, variant, entry)).items():
                name = asset_stem(model, variant) + '.latest.' + format
                target = Path(directory) / name
                if target.exists() and target.read_bytes() != data:
                    raise ValueError('immutable device manifest conflict: ' + name)
                target.write_bytes(data)
                pair[format] = dict(path='/releases/' + release['tag'] + '/' + name,
                                    size=len(data), sha256=hashlib.sha256(data).hexdigest())
            manifests[model + '/' + variant] = pair
    release['device_manifests'] = manifests


def verify_manifests(release, fetch):
    """Check both formats against canonical values, including their size/hash index.

    Old immutable releases predate these assets and remain untouched.
    """
    if 'device_manifests' not in release:
        return
    expected_keys = set()
    for model, original in release['models'].items():
        for variant, entry in [('full', original), *original.get('variants', {}).items()]:
            if 'signed' not in entry['downloads']:
                continue
            key = model + '/' + variant
            expected_keys.add(key)
            pair = release['device_manifests'].get(key, {})
            if set(pair) != {'json', 'msgpack'}:
                raise ValueError('Device manifest must index both formats: ' + key)
            for extension, expected in encoded(document(release, model, variant, entry)).items():
                asset = pair[extension]
                path = '/releases/' + release['tag'] + '/' + asset_stem(model, variant) + '.latest.' + extension
                if asset != dict(path=path, size=len(expected), sha256=hashlib.sha256(expected).hexdigest()):
                    raise ValueError('Device manifest metadata differs from canonical values: ' + key)
                if fetch(path) != expected:
                    raise ValueError('Device manifest content differs from canonical values: ' + key)
    if set(release['device_manifests']) != expected_keys:
        raise ValueError('Device manifest model/variant set differs from release')
