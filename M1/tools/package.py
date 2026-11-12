#!/usr/bin/env python3
import os
import hashlib
import json
import shutil
import sys
from pathlib import Path
from check_image import check
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from check_version import project_version, build_metadata

source, output, sdk = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3]
variant = os.environ.get('PHICOMM_FIRMWARE_VARIANT', 'full')
if variant not in ('full', 'no-frontend-resources'): raise ValueError('Unknown variant')
artifact = "phicomm_m1" + ('' if variant == 'full' else '-no-frontend-resources')
output.mkdir(parents=True, exist_ok=True)
# A newly packaged raw image invalidates any envelope produced for the prior image.
(output / (artifact + '.ota.bin.signed')).unlink(missing_ok=True)
# Explicit artifact list; full image is packaged on request and contains ATE.
prefix = os.environ.get('PHICOMM_M1_BUILD_TARGET', 'M1@MK3080B@moc')
if prefix not in ('M1@MK3080B@moc', 'M1@MK3080B@moc@release'):
    raise ValueError('Unknown M1 build target')
version = project_version()
version_metadata = build_metadata()
manifest = {"version": version, "sdk_commit": sdk, "target": prefix, "toolchain": "ARM GCC "+os.environ.get("PHICOMM_BUILD_COMPILER_EXPECTED", "5.4.1"), "files": {}}
manifest.update(version_metadata)
manifest['sdk'] = json.loads((Path(__file__).resolve().parents[2] / 'common/mico-sdk.json').read_text())
manifest['firmware_variant'] = variant
manifest['sdk_build_type'] = os.environ.get('PHICOMM_M1_BUILD_TYPE', 'release_log')
manifest['frontend_http_queue_sha256'] = hashlib.sha256((Path(__file__).resolve().parents[2] / 'common/web/http_queue.js').read_bytes()).hexdigest()
frontend_root = Path(__file__).resolve().parents[2]
frontend_metadata = frontend_root / 'build/frontend-m1-provenance.json'
manifest['frontend'] = json.loads((frontend_metadata if frontend_metadata.exists() else frontend_root / 'frontend.lock.json').read_text())
for suffix in (".usr.bin", ".ota.bin", ".all.bin", ".elf", ".map"):
    original = source / (prefix + suffix)
    if not original.is_file():
        raise SystemExit(f"Missing required artifact: {original}")
    if original.exists():
        destination = output / (artifact + suffix)
        shutil.copyfile(original, destination)
        manifest["files"][destination.name] = hashlib.sha256(destination.read_bytes()).hexdigest()
manifest["sdk_patch_sha256"] = hashlib.sha256((Path(__file__).resolve().parents[1] / "sdk.patch").read_bytes()).hexdigest()
manifest["features_sdk_patch_sha256"] = hashlib.sha256((Path(__file__).resolve().parents[1] / "features-sdk.patch").read_bytes()).hexdigest()
manifest["image"] = check(output / (artifact + '.ota.bin'))
if (version.encode() + b"\0") not in (output / (artifact + '.usr.bin')).read_bytes():
    raise SystemExit("Built image does not contain project version")
(output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
(output / "SHA256SUMS").write_text("".join(f"{digest}  {name}\n" for name, digest in manifest["files"].items()))
print(f"M1 artifacts: {output}")
