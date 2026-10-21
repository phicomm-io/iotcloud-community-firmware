#!/usr/bin/env python3
"""Host sanitizer tests for adversarial inputs through the production HTTP core."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class SharedHTTPAdversarial(unittest.TestCase):
    def test_production_core_under_adversarial_io(self):
        with tempfile.TemporaryDirectory() as temp:
            exe = Path(temp) / "adversarial"
            subprocess.run(
                [
                    os.environ.get("CC", "cc"),
                    "-std=c11", "-Wall", "-Wextra", "-Werror", "-g",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    "-no-pie", "-I" + str(ROOT / "common/httpd"),
                    str(ROOT / "tests/http_core/adversarial.c"),
                    str(ROOT / "common/httpd/core.c"),
                    str(ROOT / "common/httpd/parser.c"), "-o", str(exe),
                ],
                check=True,
            )
            subprocess.run([str(exe)], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
