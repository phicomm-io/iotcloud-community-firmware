#!/usr/bin/env python3
"""Sustained fake-I/O soak against the actual shared HTTP core and parser."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
import re


ROOT = Path(__file__).resolve().parents[1]
SOURCE_ROOT = Path(os.environ.get("PH_HTTP_SOURCE_ROOT", ROOT)).resolve()
SECONDS = os.environ.get("PH_HTTP_SOAK_SECONDS", "2")
ROUNDS = os.environ.get("PH_HTTP_SOAK_ROUNDS", "25000")
SEED = os.environ.get("PH_HTTP_SOAK_SEED", "0x51a7e")


def parse_bounded_int(name, value, minimum, maximum, base=10):
    try:
        parsed = int(value, base)
    except ValueError as exc:
        raise ValueError(f"{name} must be an integer") from exc
    if not minimum <= parsed <= maximum:
        raise ValueError(f"{name} must be between {minimum} and {maximum}")
    return parsed


class SharedHTTPSoakTest(unittest.TestCase):
    def test_mixed_lifecycle_soak(self):
        seconds = parse_bounded_int("PH_HTTP_SOAK_SECONDS", SECONDS, 1, 14400)
        rounds = parse_bounded_int("PH_HTTP_SOAK_ROUNDS", ROUNDS, 1, 10000000)
        seed = parse_bounded_int("PH_HTTP_SOAK_SEED", SEED, 0, 0xFFFFFFFF, 0)
        httpd = SOURCE_ROOT / "common/httpd"
        self.assertTrue((httpd / "http.h").is_file(), httpd)
        with tempfile.TemporaryDirectory() as temp:
            exe = Path(temp) / "shared-http-soak"
            subprocess.run(
                [
                    os.environ.get("CC", "cc"),
                    "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    "-no-pie", "-I" + str(httpd),
                    str(ROOT / "tests/http_core/soak.c"),
                    str(httpd / "core.c"), str(httpd / "parser.c"),
                    "-o", str(exe),
                ],
                check=True,
            )
            result = subprocess.run(
                [str(exe), "--seconds", str(seconds),
                 "--min-rounds", str(rounds), "--seed", str(seed)],
                check=False, capture_output=True, text=True,
                timeout=max(300, seconds + 120),
            )
            print(result.stdout, end="")
            if result.returncode:
                print(result.stderr, end="")
            self.assertEqual(result.returncode, 0)
            self.assertIn("shared HTTP soak PASS", result.stdout)
            self.assertIn("healthy_final=1", result.stdout)
            for metric in ("waiting_full", "body_lease", "wait_lease",
                           "queued_ok", "busy_503", "busy_send_error",
                           "body_complete", "wait_retry"):
                self.assertRegex(result.stdout, rf"{metric}=[1-9][0-9]*")


if __name__ == "__main__":
    unittest.main()
