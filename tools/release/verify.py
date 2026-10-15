#!/usr/bin/env python3
"""Offline verify the production envelope against the embedded keyring."""
import hashlib,re,struct
from pathlib import Path
from cryptography.hazmat.primitives.asymmetric import ec,utils
from cryptography.hazmat.primitives import hashes
DOMAIN=b'PHICOMM-OTA-V1'
def verify(body, model, version, commit):
 if len(body)<192 or body[:8]!=b'PHOTA001':raise ValueError('missing signed envelope')
 _,abi,header,key,product,native,size,counter,digest,ver,sha,channel,reserved=struct.unpack('<8sHHIHHIQ32s32s20sB11s',body[:128])
 if abi!=1 or header!=128 or product!={'tc1':1,'m1':2,'a1':3}[model] or size!=len(body)-192:raise ValueError('envelope identity or length mismatch')
 if digest!=hashlib.sha256(body[192:]).digest() or ver.rstrip(b'\0').decode().removeprefix('v')!=version.removeprefix('v') or sha.hex()!=commit:raise ValueError('envelope digest, version or commit mismatch')
 text=(Path(__file__).resolve().parents[2]/'common/ota/keyring.h').read_text();match=re.search(r'\{'+str(key)+r', \{([^}]+)\}',text)
 if not match:raise ValueError('untrusted signing key')
 raw=bytes(int(x,16) for x in re.findall(r'0x[0-9a-f]{2}',match[1]));public=ec.EllipticCurvePublicNumbers(int.from_bytes(raw[:32],'big'),int.from_bytes(raw[32:],'big'),ec.SECP256R1()).public_key()
 public.verify(utils.encode_dss_signature(int.from_bytes(body[128:160],'big'),int.from_bytes(body[160:192],'big')),DOMAIN+body[:128],ec.ECDSA(hashes.SHA256()))
if __name__=='__main__':
 import argparse
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('file',type=Path);p.add_argument('model',choices=['tc1','m1','a1']);p.add_argument('version');p.add_argument('commit');a=p.parse_args();verify(a.file.read_bytes(),a.model,a.version,a.commit)
