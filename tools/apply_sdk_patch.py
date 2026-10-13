#!/usr/bin/env python3
"""Apply a versioned SDK overlay once, rejecting incompatible cache contents."""
import subprocess
import sys
sdk, patch = sys.argv[1:]
check = subprocess.run(['git', 'apply', '--reverse', '--check', patch], cwd=sdk,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
if check.returncode:
    subprocess.run(['git', 'apply', '--check', patch], cwd=sdk, check=True)
    subprocess.run(['git', 'apply', patch], cwd=sdk, check=True)
