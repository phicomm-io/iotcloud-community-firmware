"""Build-time fixture text -> constant MP vectors; no parser in the C binary.

Existing human-readable request vectors remain reviewable. This module runs on
host Python before compilation; product fixtures link only typed_wire/buffer.
"""
import ast
import json
import re
import struct

_LITERAL = r'"(?:\\.|[^"\\])*"'
_SEQUENCE = re.compile(_LITERAL + r'(?:\s*(?:CONTROL_PARAMS|CONTROL|KEY|' + _LITERAL + r'))*')


def encode(value):
    if value is None: return b'\xc0'
    if value is True: return b'\xc3'
    if value is False: return b'\xc2'
    if isinstance(value, int):
        if 0 <= value < 128: return bytes([value])
        if -32 <= value < 0: return bytes([value & 255])
        for code, fmt, low, high in ((0xcc, '>B', 0, 255), (0xcd, '>H', 0, 65535), (0xce, '>I', 0, 2**32-1), (0xd0, '>b', -128, 127), (0xd1, '>h', -32768, 32767), (0xd2, '>i', -2**31, 2**31-1), (0xd3, '>q', -2**63, 2**63-1)):
            if low <= value <= high: return bytes([code]) + struct.pack(fmt, value)
        raise ValueError(value)
    if isinstance(value, float): return b'\xcb' + struct.pack('>d', value)
    if isinstance(value, str):
        b = value.encode('utf8')
        return (bytes([0xa0 + len(b)]) if len(b) < 32 else b'\xda' + struct.pack('>H', len(b))) + b
    if isinstance(value, list): return b'\xdc' + struct.pack('>H', len(value)) + b''.join(map(encode, value))
    if isinstance(value, dict): return b'\xde' + struct.pack('>H', len(value)) + b''.join(encode(k) + encode(v) for k, v in value.items())
    raise TypeError(type(value))


def header(sources, macros):
    requests = {}
    for source in sources:
        for match in _SEQUENCE.finditer(source):
            tokens = re.findall(_LITERAL + r'|CONTROL_PARAMS|CONTROL|KEY', match.group())
            text = ''.join(macros[t] if t in macros else ast.literal_eval(t) for t in tokens)
            if not text.startswith('{'): continue
            try: value = json.loads(text)
            except json.JSONDecodeError: continue  # printf vectors use direct typed writers.
            requests[text] = encode(value)
    declarations, rows = [], []
    for i, (text, wire) in enumerate(requests.items()):
        declarations.append('static const unsigned char fixture_wire_%u[]={%s};' % (i, ','.join(str(x) for x in wire)))
        rows.append('{%s,fixture_wire_%u,sizeof(fixture_wire_%u)}' % (json.dumps(text, ensure_ascii=True), i, i))
    return '\n'.join(declarations) + '\nstatic const struct {const char *text;const unsigned char *bytes;size_t length;} fixture_vectors[]={\n' + ',\n'.join(rows) + '\n};\n'
