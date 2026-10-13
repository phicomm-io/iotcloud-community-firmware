#!/usr/bin/env python3
"""Restore the pinned TC1 SDK archive into the local build cache."""
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SDK = json.loads((ROOT / 'TC1/sdk.json').read_text())


def prepare(destination):
    destination = Path(destination).resolve()
    destination.parent.mkdir(parents=True, exist_ok=True)
    with (destination.parent / '.sdk.lock').open('w') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        marker = destination / '.phicomm-sdk-sha256'
        if marker.is_file() and marker.read_text().strip() == SDK['sha256']:
            if not (destination / 'makefiles/Makefile').is_file():
                raise SystemExit('TC1 SDK cache is incomplete; choose an empty TC1_SDK_DIR')
            return
        if destination.exists():
            raise SystemExit('TC1 SDK cache does not match sdk.json; choose an empty TC1_SDK_DIR')
        archive = Path(os.environ.get('TC1_SDK_ARCHIVE', destination.parent / SDK['asset'])).resolve()
        archive.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='tc1-sdk-', dir=destination.parent) as temporary:
            temporary = Path(temporary)
            if not archive.is_file():
                if 'TC1_SDK_ARCHIVE' in os.environ:
                    raise SystemExit('TC1_SDK_ARCHIVE does not exist')
                downloaded = temporary / SDK['asset']
                authenticated = os.environ.get('GH_TOKEN') or os.environ.get('GITHUB_TOKEN')
                if shutil.which('gh') and (authenticated or subprocess.run(
                    ['gh', 'auth', 'status'], stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL).returncode == 0):
                    subprocess.run(['gh', 'release', 'download', SDK['release'],
                                    '--repo', SDK['repository'], '--pattern', SDK['asset'],
                                    '--output', str(downloaded)], check=True)
                else:
                    subprocess.run(['curl', '-fsSL', '--retry', '3', SDK['url'],
                                    '-o', str(downloaded)], check=True)
                if digest(downloaded) != SDK['sha256']:
                    raise SystemExit('Downloaded TC1 SDK SHA256 mismatch')
                shutil.move(downloaded, archive)
            if digest(archive) != SDK['sha256']:
                raise SystemExit('TC1 SDK archive SHA256 mismatch')
            # The archive is accepted only after checking its pinned checksum.
            extracted = temporary / 'extracted'
            extracted.mkdir()
            subprocess.run(['tar', '-xzf', str(archive), '-C', str(extracted),
                            'tc1-sdk/mico-os'], check=True)
            source = extracted / 'tc1-sdk/mico-os'
            if not (source / 'makefiles/Makefile').is_file():
                raise SystemExit('TC1 SDK archive layout mismatch')
            (source / '.phicomm-sdk-sha256').write_text(SDK['sha256'] + '\n')
            shutil.move(source, destination)


def digest(path):
    checksum = hashlib.sha256()
    with path.open('rb') as source:
        for block in iter(lambda: source.read(1024 * 1024), b''):
            checksum.update(block)
    return checksum.hexdigest()


if __name__ == '__main__':
    prepare(sys.argv[1])
