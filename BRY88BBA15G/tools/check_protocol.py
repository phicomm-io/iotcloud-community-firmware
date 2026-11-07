#!/usr/bin/env python3
"""Validate the pinned protocol snapshot, and both repositories if supplied."""
from pathlib import Path
import hashlib
import json
import re
import sys

root=Path(__file__).resolve().parents[1]
lock=json.loads((root/'screen-protocol.lock.json').read_text())
opcodes={}
for name, expected in lock['files'].items():
    data=(root/name).read_bytes()
    if hashlib.sha256(data).hexdigest()!=expected:
        raise SystemExit(f'Protocol snapshot mismatch: {name}')
    for symbol,value in re.findall(rb'#define\s+(SCREEN_MSG_\w+)\s+(0x[0-9a-fA-F]+)u?',data):
        code=int(value,16)
        if code in opcodes and opcodes[code]!=symbol:
            raise SystemExit(f'Duplicate screen opcode {code:#x}: {opcodes[code]} and {symbol}')
        opcodes[code]=symbol
    if len(sys.argv)>1:
        source=Path(sys.argv[1])/lock['source_paths'][name]
        if source.read_bytes()!=data:
            raise SystemExit(f'Cross-repository protocol mismatch: {source}')
print('Protocol snapshot verified')
