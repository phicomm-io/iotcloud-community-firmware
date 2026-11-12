#!/usr/bin/env python3
"""Provision pinned MXCHIP build profiles in the shared Phicomm cache."""
import argparse
import fcntl
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
from urllib.request import urlopen

ROOT = Path(__file__).resolve().parents[1]
GCC_DIR = 'gcc-arm-none-eabi-5_4-2016q2'
ARCHIVES = {
    'gcc5': ('https://launchpad.net/gcc-arm-embedded/5.0/5-2016-q2-update/+download/'
             'gcc-arm-none-eabi-5_4-2016q2-20160622-linux.tar.bz2',
             '9910b6b5df12efe564dbb3856bf1599d4c16178a6f28bd8a23c9e5c3edc219e4', GCC_DIR),
    'gcc14': ('https://developer.arm.com/-/media/Files/downloads/gnu/14.3.rel1/binrel/'
              'arm-gnu-toolchain-14.3.rel1-x86_64-arm-none-eabi.tar.xz',
              '8f6903f8ceb084d9227b9ef991490413014d991874a1e34074443c2a72b14dbd',
              'arm-gnu-toolchain-14.3.rel1-x86_64-arm-none-eabi'),
    'python2': ('https://www.python.org/ftp/python/2.7.18/Python-2.7.18.tgz',
                'da3080e3b488f648a3d7a4560ddee895284c3380b11d6de75edb986526b9a814', 'Python-2.7.18'),
}


def compiler_version(directory):
    return subprocess.check_output([str(directory / 'bin/arm-none-eabi-gcc'), '-dumpversion'], text=True).strip()


def sources(name, temporary):
    url, expected, top = ARCHIVES[name]
    archive = temporary / 'archive'
    digest = hashlib.sha256()
    with urlopen(url, timeout=120) as response, archive.open('wb') as output:
        while True:
            block = response.read(1024 * 1024)
            if not block:
                break
            digest.update(block)
            output.write(block)
    if digest.hexdigest() != expected:
        raise ValueError('Tool archive SHA-256 mismatch: ' + name)
    unpacked = temporary / 'unpacked'
    unpacked.mkdir()
    with tarfile.open(archive) as tar:
        for member in tar.getmembers():
            path = Path(member.name)
            if path.is_absolute() or '..' in path.parts or not path.parts or path.parts[0] != top:
                raise ValueError('Unexpected tool archive path: ' + member.name)
        tar.extractall(unpacked, filter='data')
    archive.unlink()
    return unpacked / top


def link(target, destination):
    if destination.is_symlink() and destination.resolve() == target.resolve():
        return
    if destination.exists() or destination.is_symlink():
        raise ValueError('Conflicting shared profile path: ' + str(destination))
    destination.symlink_to(target)


def profile(home, name):
    selected = home / 'profiles' / name
    selected.mkdir(parents=True, exist_ok=True)
    link(home / 'python2', selected / 'python2')
    if name == 'gcc5':
        link(home / 'gcc5', selected / GCC_DIR)
    else:
        binaries = selected / GCC_DIR / 'bin'
        binaries.mkdir(parents=True, exist_ok=True)
        for binary in (home / 'gcc5/bin').iterdir():
            if binary.name in ('arm-none-eabi-gcc', 'arm-none-eabi-g++'):
                destination = binaries / binary.name
                temporary = binaries / (binary.name + '.new')
                source = (ROOT / 'tools/compiler_profile.py').read_text().split('\n', 1)[1]
                # MiCO replaces PATH with its own MiCoder command directory.
                temporary.write_text('#!' + str(Path(sys.executable).resolve()) + '\n' + source)
                temporary.chmod(0o755)
                temporary.replace(destination)
            else:
                link(binary, binaries / binary.name)
    return selected


def prepare(home, name, logs):
    home.mkdir(parents=True, exist_ok=True)
    logs.mkdir(parents=True, exist_ok=True)
    with (home / '.toolchain.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        for compiler, expected in [('gcc5', '5.4.1')] + ([('gcc14', '14.3.1')] if name == 'gcc14' else []):
            destination = home / compiler
            if not destination.exists():
                with tempfile.TemporaryDirectory(prefix='toolchain-', dir=home) as tmp:
                    sources(compiler, Path(tmp)).rename(destination)
            if compiler_version(destination) != expected:
                raise ValueError('Incompatible shared compiler: ' + str(destination))
        python = home / 'python2/bin/python2.7'
        if not python.exists():
            if (home / 'python2').exists():
                raise ValueError('Incomplete shared Python: ' + str(home / 'python2'))
            with tempfile.TemporaryDirectory(prefix='python2-', dir=home) as tmp:
                source = sources('python2', Path(tmp))
                with (logs / 'python2-build.log').open('w') as output:
                    for command in [['./configure', '--prefix=' + str(home / 'python2')],
                                    ['make', '-j4'], ['make', 'install']]:
                        subprocess.run(command, cwd=source, stdout=output, stderr=subprocess.STDOUT, check=True)
        subprocess.run([str(python), '-c', 'import os,sys; assert sys.version_info[:3]==(2,7,18); '
                        'assert os.path.realpath(sys.prefix)==os.path.realpath(sys.argv[1])', str(home / 'python2')], check=True)
        return profile(home, name)


def environment(home, selected, name, logs):
    values = {'PHICOMM_TOOLS_HOME': str(home),
              'PHICOMM_BUILD_COMPILER_EXPECTED': '14.3.1' if name == 'gcc14' else '5.4.1',
              'ARM_CC': str(selected / GCC_DIR / 'bin/arm-none-eabi-gcc'),
              'PHICOMM_COMPILER_LOG': str(logs / (name + '-compiler.jsonl'))}
    for model in ('A1', 'M1', 'TC1'):
        values[model + '_TOOLS_DIR'] = str(selected)
        values[model + '_PYTHON2'] = str(home / 'python2/bin/python2.7')
    return values


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile', choices=('gcc5', 'gcc14'), default='gcc14')
    parser.add_argument('--home', type=Path, default=Path(os.environ.get('PHICOMM_TOOLS_HOME',
        str(Path(os.environ.get('XDG_CACHE_HOME', Path.home() / '.cache')) / 'phicomm'))))
    parser.add_argument('--log-dir', type=Path, default=ROOT / 'build/toolchain-logs')
    parser.add_argument('--github-env', type=Path)
    args = parser.parse_args()
    home, logs = args.home.resolve(), args.log_dir.resolve()
    selected = prepare(home, args.profile, logs)
    values = environment(home, selected, args.profile, logs)
    if args.github_env:
        with args.github_env.open('a') as output:
            for key, value in values.items():
                output.write(key + '=' + value + '\n')
    for key, value in values.items():
        print(key + '=' + value)
