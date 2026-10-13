#!/usr/bin/env python3
import hashlib
import json
import shutil
import sys
from pathlib import Path
from check_image import check

source, output, sdk = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3]
output.mkdir(parents=True, exist_ok=True)
# Explicit artifact list; full image is packaged on request and contains ATE.
prefix = "M1@MK3080B@moc"
manifest = {"sdk_commit": sdk, "target": prefix, "toolchain": "ARM GCC 5.4.1", "files": {}}
for suffix in (".usr.bin", ".ota.bin", ".all.bin", ".elf", ".map"):
    original = source / (prefix + suffix)
    if not original.is_file():
        raise SystemExit(f"Missing required artifact: {original}")
    if original.exists():
        destination = output / ("phicomm_m1" + suffix)
        shutil.copyfile(original, destination)
        manifest["files"][destination.name] = hashlib.sha256(destination.read_bytes()).hexdigest()
manifest["sdk_patch_sha256"] = hashlib.sha256((Path(__file__).resolve().parents[1] / "sdk.patch").read_bytes()).hexdigest()
manifest["image"] = check(output / "phicomm_m1.ota.bin")
(output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
(output / "SHA256SUMS").write_text("".join(f"{digest}  {name}\n" for name, digest in manifest["files"].items()))
print(f"M1 artifacts: {output}")
