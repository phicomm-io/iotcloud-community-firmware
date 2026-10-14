#!/usr/bin/env python3
"""Canonical OTA V1 signing. Private keys must never be stored in this repository."""
import argparse
import hashlib
import re
import struct
from pathlib import Path
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec, utils
DOMAIN = b'PHICOMM-OTA-V1'
MODELS = {'tc1': 1, 'm1': 2, 'a1': 3}

def header(image, model, version, commit, key_id=1, channel=1):
    match = re.fullmatch(r'v?(\d+)\.(\d+)\.(\d+)', version)
    if not match:
        raise ValueError('Release version must be MAJOR.MINOR.PATCH')
    parts = [int(x) for x in match.groups()]
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
    keyring = (Path(__file__).resolve().parents[2] / 'common/ota/keyring.h').read_text()
    trusted = re.search(r'\{' + str(args.key_id) + r', \{([^}]+)\}', keyring)
    if not trusted or bytes(int(x, 16) for x in re.findall(r'0x[0-9a-f]{2}', trusted[1])) != raw:
        p.error('Signing key does not match the firmware trusted key_id')
    package = sign(args.input.read_bytes(), args.model, args.version, args.commit, key, args.key_id)
    r, signature_s = int.from_bytes(package[128:160], 'big'), int.from_bytes(package[160:192], 'big')
    key.public_key().verify(utils.encode_dss_signature(r, signature_s),
                            DOMAIN + package[:128], ec.ECDSA(hashes.SHA256()))
    if hashlib.sha256(package[192:]).digest() != package[32:64]:
        raise ValueError('Signed package digest self-check failed')
    args.output.write_bytes(package)

if __name__ == '__main__':
    main()
