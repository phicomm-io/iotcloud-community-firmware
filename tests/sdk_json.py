"""Disposable JSON overlay copy for pristine or already patched pinned SDKs."""
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def copy_patched_json(source, destination, checked_numeric=True):
    destination = Path(destination)
    json = destination/'libraries/utilities/json_c'
    json.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(source, json)
    patches = ('json-newlib-reent.patch', 'json-printbuf-oom.patch',
               'json-allocation-safety.patch', 'json-heap-budget.patch',
               'json-newlib-nano-abi.patch', 'json-integer-parse.patch',
               'json-sized-resize.patch', 'json-number-cache-cleanup.patch')
    patches += ('json-number-checked.patch',)
    subprocess.run([sys.executable, str(ROOT/'tools/apply_sdk_patch.py'),
                    str(destination), *[str(ROOT/'common'/p) for p in patches]],
                   check=True, timeout=30)
    subprocess.run([sys.executable, str(ROOT/'tools/install_json_numeric.py'),
                    str(destination)], check=True, timeout=30)
    if not checked_numeric:
        # Historical native negative controls, even when source SDK already
        # carries the current checked overlay. This copy is disposable.
        subprocess.run(['git', 'apply', '--ignore-whitespace', '--reverse',
                        str(ROOT/'common/json-number-checked.patch')],
                       cwd=destination, check=True, timeout=30)
    return json
