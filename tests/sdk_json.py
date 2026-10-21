"""Disposable JSON overlay copy for pristine or already patched pinned SDKs."""
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def copy_patched_json(source, destination):
    destination = Path(destination)
    json = destination/'libraries/utilities/json_c'
    json.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(source, json)
    patches = ('json-newlib-reent.patch', 'json-printbuf-oom.patch',
               'json-allocation-safety.patch', 'json-heap-budget.patch',
               'json-newlib-nano-abi.patch', 'json-integer-parse.patch',
               'json-sized-resize.patch')
    subprocess.run([sys.executable, str(ROOT/'tools/apply_sdk_patch.py'),
                    str(destination), *[str(ROOT/'common'/p) for p in patches]],
                   check=True, timeout=30)
    return json
