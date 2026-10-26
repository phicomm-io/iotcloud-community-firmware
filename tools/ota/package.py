#!/usr/bin/env python3
"""Canonical OTA V1 signing. Private keys must never be stored in this repository."""
import argparse
import hashlib
import re
import struct
import sys
from pathlib import Path
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec, utils
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'release'))
from release_version import parse_version
DOMAIN = b'PHICOMM-OTA-V1'
MODELS = {'tc1': 1, 'm1': 2, 'a1': 3, 'daikin': 4}

def header(image, model, version, commit, key_id=1, channel=None):
    identity = parse_version(version)
    parts = identity.core
    if channel is None:
        channel = 2 if identity.prerelease else 1
    if identity.prerelease and channel != 2:
        raise ValueError('Release candidates require signed OTA candidate channel 2')
    if any(x >= 1 << 20 for x in parts):
        raise ValueError('Version component exceeds 20 bits')
    version = version.encode('ascii')
    if not 0 < len(version) < 32 or not re.fullmatch(r'[0-9a-fA-F]{40}', commit):
        raise ValueError('Invalid version or Git SHA-1')
    if model not in MODELS or channel not in (1, 2) or not 0 < key_id < 1 << 32:
        raise ValueError('Invalid package identity')
    counter = parts[0] << 40 | parts[1] << 20 | parts[2]
    return struct.pack('<8sHHIHHIQ32s32s20sB11s', b'PHOTA001', 1, 128,
                       key_id, MODELS[model], 1, len(image), counter,
                       hashlib.sha256(image).digest(), version.ljust(32, b'\0'),
                       bytes.fromhex(commit), channel, bytes(11))

def sign(image, model, version, commit, key, key_id=1):
    if not isinstance(key, ec.EllipticCurvePrivateKey) or not isinstance(key.curve, ec.SECP256R1):
        raise ValueError('Expected a P-256 private key')
    h = header(image, model, version, commit, key_id)
    r, s = utils.decode_dss_signature(key.sign(DOMAIN + h, ec.ECDSA(hashes.SHA256())))
    # Canonical low-S signatures (verifier also accepts mathematically valid high-S).
    order = 0xffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551
    s = min(s, order - s)
    return h + r.to_bytes(32, 'big') + s.to_bytes(32, 'big') + image

def trusted_public_key(key_id=1, keyring_path=None):
    keyring_path = keyring_path or Path(__file__).resolve().parents[2] / 'common/ota/keyring.h'
    keyring = Path(keyring_path).read_text()
    matches = re.findall(r'\{\s*' + str(key_id) + r'\s*,\s*\{([^}]+)\}\s*\}', keyring)
    if len(matches) != 1:
        raise ValueError(f'Expected one trusted OTA key with id {key_id}')
    values = re.findall(r'0x([0-9a-fA-F]{2})', matches[0])
    if len(values) != 64:
        raise ValueError(f'Invalid trusted OTA key with id {key_id}')
    raw = bytes(int(value, 16) for value in values)
    numbers = ec.EllipticCurvePublicNumbers(
        int.from_bytes(raw[:32], 'big'), int.from_bytes(raw[32:], 'big'), ec.SECP256R1())
    return numbers.public_key()

def verify_package(package, model, keyring_path=None):
    if model not in MODELS or len(package) < 192:
        raise ValueError('Invalid model or truncated signed OTA package')
    header = package[:128]
    magic, fmt, header_size, key_id, model_id, hardware, length, counter, digest, version, commit, channel, reserved = struct.unpack(
        '<8sHHIHHIQ32s32s20sB11s', header)
    if (magic != b'PHOTA001' or fmt != 1 or header_size != 128 or
            model_id != MODELS[model] or hardware != 1 or length != len(package) - 192 or
            channel not in (1, 2) or reserved != bytes(11)):
        raise ValueError('Signed OTA header does not match package or target model')
    version_raw = version.split(b'\0', 1)[0]
    if not version_raw or version_raw + bytes(32 - len(version_raw)) != version:
        raise ValueError('Invalid signed OTA version field')
    try:
        version_text = version_raw.decode('ascii')
        identity = parse_version(version_text)
    except (UnicodeDecodeError, ValueError) as error:
        raise ValueError('Invalid signed OTA version') from error
    expected_counter = sum(part << shift for part, shift in zip(identity.core, (40, 20, 0)))
    if counter != expected_counter or any(part >= 1 << 20 for part in identity.core):
        raise ValueError('Signed OTA version counter mismatch')
    if identity.prerelease and channel != 2:
        raise ValueError('Signed OTA release candidate is not in candidate channel 2')
    payload = package[192:]
    if hashlib.sha256(payload).digest() != digest:
        raise ValueError('Signed OTA payload digest mismatch')
    signature = utils.encode_dss_signature(int.from_bytes(package[128:160], 'big'),
                                           int.from_bytes(package[160:192], 'big'))
    try:
        trusted_public_key(key_id, keyring_path).verify(
            signature, DOMAIN + header, ec.ECDSA(hashes.SHA256()))
    except Exception as error:
        raise ValueError('Signed OTA signature is not trusted or is invalid') from error
    return {'key_id': key_id, 'model': model, 'version': version_text,
            'channel': channel, 'prerelease': identity.prerelease,
            'commit': commit.hex(), 'payload_size': length,
            'payload_sha256': digest.hex()}

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--key', type=Path, required=True)
    p.add_argument('--model', choices=MODELS, required=True)
    p.add_argument('--version', required=True)
    p.add_argument('--commit', required=True)
    p.add_argument('--key-id', type=int, default=1)
    p.add_argument('input', type=Path)
    p.add_argument('output', type=Path)
    args = p.parse_args()
    if args.key.resolve().is_relative_to(Path(__file__).resolve().parents[2]):
        p.error('Private key must be outside the source repository')
    key = serialization.load_pem_private_key(args.key.read_bytes(), None)
    public = key.public_key().public_numbers()
    raw = public.x.to_bytes(32, 'big') + public.y.to_bytes(32, 'big')
    trusted = trusted_public_key(args.key_id).public_numbers()
    if (trusted.x.to_bytes(32, 'big') + trusted.y.to_bytes(32, 'big')) != raw:
        p.error('Signing key does not match the firmware trusted key_id')
    package = sign(args.input.read_bytes(), args.model, args.version, args.commit, key, args.key_id)
    verify_package(package, args.model)
    args.output.write_bytes(package)

if __name__ == '__main__':
    main()
