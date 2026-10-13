#!/usr/bin/env python3
"""Generate a deterministic, offline gzip page; writes only ignored artifacts."""
import gzip
from pathlib import Path

app = Path(__file__).resolve().parents[1]
data = gzip.compress((app / "web/index.html").read_bytes(), compresslevel=9, mtime=0)
output = app / "generated"
output.mkdir(exist_ok=True)
lines = ["#pragma once", "static const unsigned char a1_web_data[] = {"]
for i in range(0, len(data), 16):
    lines.append("    " + ",".join(f"0x{b:02x}" for b in data[i:i + 16]) + ",")
lines.append("};\n")
(output / "web_data.h").write_text("\n".join(lines))
print(f"A1 offline UI: {len(data)} bytes gzip")
