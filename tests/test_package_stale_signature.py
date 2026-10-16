import hashlib
import json
import runpy
import sys
import tempfile
import types
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class PackageStaleSignatureTest(unittest.TestCase):
    def test_repackaging_removes_only_own_old_envelope_and_updates_hashes(self):
        checker = types.ModuleType("check_image")
        checker.check = lambda path: {"sha256": hashlib.sha256(Path(path).read_bytes()).hexdigest()}
        old_checker = sys.modules.get("check_image")
        sys.modules["check_image"] = checker
        try:
            for model in ("a1", "m1"):
                with self.subTest(model=model), tempfile.TemporaryDirectory() as temp:
                    root = Path(temp)
                    source, output = root / "source", root / "release"
                    source.mkdir()
                    prefix = f"{model.upper()}@MK3080B@moc"
                    for suffix in (".usr.bin", ".ota.bin", ".all.bin", ".elf", ".map"):
                        payload = b"firmware " + suffix.encode()
                        if suffix == ".usr.bin":
                            from tools.check_version import project_version
                            payload += b"\0" + project_version().encode() + b"\0"
                        (source / (prefix + suffix)).write_bytes(payload)

                    signed_name = f"phicomm_{model}.ota.bin.signed"
                    signed = output / signed_name
                    unrelated = output / "phicomm_tc1.ota.bin.signed"
                    sentinel = output / "keep-me.txt"
                    output.mkdir()
                    signed.write_bytes(b"old signed envelope")
                    unrelated.write_bytes(b"other product envelope")
                    sentinel.write_bytes(b"unrelated output")

                    script = ROOT / model.upper() / "tools" / "package.py"
                    old_argv = sys.argv
                    try:
                        sys.argv = [str(script), str(source), str(output), "test-sdk"]
                        runpy.run_path(str(script), run_name="__main__")
                    finally:
                        sys.argv = old_argv

                    self.assertFalse(signed.exists())
                    self.assertEqual(unrelated.read_bytes(), b"other product envelope")
                    self.assertEqual(sentinel.read_bytes(), b"unrelated output")
                    manifest = json.loads((output / "manifest.json").read_text())
                    raw_name = f"phicomm_{model}.ota.bin"
                    self.assertEqual(manifest["files"][raw_name], hashlib.sha256((output / raw_name).read_bytes()).hexdigest())
                    sums = dict(line.split("  ", 1)[::-1] for line in (output / "SHA256SUMS").read_text().splitlines())
                    self.assertEqual(sums, manifest["files"])
                    self.assertEqual(manifest["image"]["sha256"], hashlib.sha256((output / raw_name).read_bytes()).hexdigest())
        finally:
            if old_checker is None:
                sys.modules.pop("check_image", None)
            else:
                sys.modules["check_image"] = old_checker


if __name__ == "__main__":
    unittest.main()
