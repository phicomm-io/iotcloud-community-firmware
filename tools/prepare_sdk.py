#!/usr/bin/env python3
"""Prepare product checkouts from one pinned MiCO source cache."""
import argparse
import fcntl
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SDK = json.loads((ROOT / 'common/mico-sdk.json').read_text())


def git(*args):
    return subprocess.check_output(['git', *map(str, args)], text=True).strip()


def verify(directory):
    if git('-C', directory, 'rev-parse', 'HEAD') != SDK['commit']:
        raise ValueError(f'MiCO SDK commit mismatch: {directory}; expected {SDK["commit"]}')


def prepare(destination):
    destination = Path(destination).resolve()
    destination.parent.mkdir(parents=True, exist_ok=True)
    with (destination.parent / '.sdk.lock').open('w') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        if destination.exists():
            if not (destination / '.git').exists():
                raise ValueError(f'Not a MiCO Git checkout: {destination}; choose an empty SDK directory')
            verify(destination)
            if not (destination / 'makefiles/Makefile').is_file():
                raise ValueError(f'Incomplete MiCO SDK checkout: {destination}')
            return
        tools_home = Path(os.environ.get('PHICOMM_TOOLS_HOME',
                          Path(os.environ.get('XDG_CACHE_HOME', Path.home() / '.cache')) / 'phicomm'))
        cache = Path(os.environ.get('MICO_SDK_CACHE', tools_home / 'mico-upstream.git')).resolve()
        cache.parent.mkdir(parents=True, exist_ok=True)
        with (cache.parent / '.mico-source.lock').open('w') as source_lock:
            fcntl.flock(source_lock, fcntl.LOCK_EX)
            if not cache.exists():
                with tempfile.TemporaryDirectory(prefix='mico-source-', dir=cache.parent) as temporary:
                    source = Path(temporary) / 'source.git'
                    git('clone', '--bare', '--depth', '1', '--branch', SDK['ref'],
                        os.environ.get('MICO_SDK_REPOSITORY', SDK['repository']), source)
                    verify(source)
                    source.rename(cache)
            verify(cache)
            with tempfile.TemporaryDirectory(prefix='mico-checkout-', dir=destination.parent) as temporary:
                checkout = Path(temporary) / 'mico-os'
                git('clone', cache, checkout)
                verify(checkout)
                checkout.rename(destination)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('destination', nargs='?')
    parser.add_argument('--commit', action='store_true')
    args = parser.parse_args()
    if args.commit:
        print(SDK['commit'])
    elif args.destination:
        try:
            prepare(args.destination)
        except ValueError as error:
            parser.exit(1, str(error) + '\n')
    else:
        parser.error('destination is required')
