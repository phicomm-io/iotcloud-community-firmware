#!/usr/bin/env python3
"""Prepare one strictly locked frontend archive for all jobs in a CI run.

Candidate source builds are an explicit workflow policy, never a release fallback
for main, tags or fork PRs. Both paths must reproduce the committed archive hash.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from urllib.error import HTTPError
from urllib.request import Request, urlopen

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from frontend_release import read_lock, unpack


def build_source(lock, source):
    """Checkout the exact object, then use its own pinned build dependencies."""
    def run(*args):
        return subprocess.run(args, cwd=source, check=True)

    source.mkdir()
    run('git', 'init', '--quiet')
    run('git', 'remote', 'add', 'origin', f"https://github.com/{lock['repository']}.git")
    run('git', 'fetch', '--depth=1', 'origin', lock['commit'])
    run('git', 'checkout', '--detach', 'FETCH_HEAD')
    actual = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=source, text=True).strip()
    if actual != lock['commit']:
        raise ValueError('Frontend source checkout does not match the locked commit')
    run(sys.executable, '-m', 'pip', 'install', '-r', 'requirements-build.txt')
    run('npm', 'ci', '--prefix', 'frontend')
    run(sys.executable, 'tools/build_release.py')
    generated = read_lock(source / 'build/frontend.lock.json')
    if generated != lock:
        raise ValueError('Rebuilt frontend lock differs from the committed lock')
    return (source / 'build/frontend.tar.gz').read_bytes()


def prepare(lock_path, output, allow_candidate_source=False):
    lock = read_lock(lock_path)
    url = f"https://github.com/{lock['repository']}/releases/download/{lock['release']}/{lock['asset']}"
    mode = 'release'
    output = Path(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.ci-frontend-', dir=output.parent) as temp:
        temp = Path(temp)
        try:
            with urlopen(Request(url, headers={'User-Agent': 'phicomm-frontend-build'}), timeout=120) as response:
                data = response.read()
        except HTTPError as error:
            # Authentication, rate limits and server failures must not change sources.
            if error.code != 404 or not allow_candidate_source:
                raise
            print('Release is not published; rebuilding the exact locked candidate commit.', flush=True)
            data = build_source(lock, temp / 'source')
            mode = 'candidate-source'
        # Always verify the archive, embedded identity, paths and every file before
        # sharing it. No previous local cache participates in dependency resolution.
        unpack(data, lock, temp / 'verified')
        staged = temp / lock['asset']
        staged.write_bytes(data)
        staged.replace(output)
    provenance = dict(schema=1, source=mode, repository=lock['repository'],
                      commit=lock['commit'], sha256=lock['sha256'], release_url=url,
                      lock_sha256=hashlib.sha256(Path(lock_path).read_bytes()).hexdigest())
    if os.environ.get('GITHUB_RUN_ID'):
        provenance['ci_run'] = (f"{os.environ['GITHUB_SERVER_URL']}/{os.environ['GITHUB_REPOSITORY']}"
                                f"/actions/runs/{os.environ['GITHUB_RUN_ID']}")
    output.with_name('provenance.json').write_text(json.dumps(provenance, indent=2) + '\n')
    shutil.copyfile(lock_path, output.with_name('frontend.lock.json'))
    print(json.dumps(provenance, indent=2))
    return provenance


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--lock', type=Path, default=Path(__file__).resolve().parents[2] / 'frontend.lock.json')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--allow-candidate-source', action='store_true')
    args = parser.parse_args()
    prepare(args.lock, args.output, args.allow_candidate_source)
