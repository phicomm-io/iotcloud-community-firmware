#!/usr/bin/env python3
import hashlib
import json
import shutil
import sys
from pathlib import Path
from check_image import check
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from check_version import project_version, build_metadata

source, output, sdk = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3]
output.mkdir(parents=True, exist_ok=True)
# A newly packaged raw image invalidates any envelope produced for the prior image.
(output / "phicomm_m1.ota.bin.signed").unlink(missing_ok=True)
# Explicit artifact list; full image is packaged on request and contains ATE.
prefix = "M1@MK3080B@moc"
version = project_version()
version_metadata = build_metadata()
manifest = {"version": version, "sdk_commit": sdk, "target": prefix, "toolchain": "ARM GCC 5.4.1", "files": {}}
manifest.update(version_metadata)
for suffix in (".usr.bin", ".ota.bin", ".all.bin", ".elf", ".map"):
    original = source / (prefix + suffix)
    if not original.is_file():
        raise SystemExit(f"Missing required artifact: {original}")
    if original.exists():
        destination = output / ("phicomm_m1" + suffix)
        shutil.copyfile(original, destination)
        manifest["files"][destination.name] = hashlib.sha256(destination.read_bytes()).hexdigest()
manifest["sdk_patch_sha256"] = hashlib.sha256((Path(__file__).resolve().parents[1] / "sdk.patch").read_bytes()).hexdigest()
manifest["features_sdk_patch_sha256"] = hashlib.sha256((Path(__file__).resolve().parents[1] / "features-sdk.patch").read_bytes()).hexdigest()
manifest["image"] = check(output / "phicomm_m1.ota.bin")
if (version.encode() + b"\0") not in (output / "phicomm_m1.usr.bin").read_bytes():
    raise SystemExit("Built image does not contain project version")
(output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
(output / "SHA256SUMS").write_text("".join(f"{digest}  {name}\n" for name, digest in manifest["files"].items()))
print(f"M1 artifacts: {output}")
