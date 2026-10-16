#!/usr/bin/env python3
"""Apply SDK overlays, validating an ordered stack before changing SDK files."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile


def environment_for(directory):
    environment = os.environ.copy()
    if not (directory / '.git').exists():
        ceiling = str(directory.parent)
        previous = environment.get('GIT_CEILING_DIRECTORIES')
        if previous:
            ceiling += os.pathsep + previous
        environment['GIT_CEILING_DIRECTORIES'] = ceiling
    return environment


def changed_paths(patch_bytes):
    paths = {line[6:].decode() for line in patch_bytes.splitlines()
             if line.startswith((b'+++ b/', b'--- a/'))}
    if not paths or any(Path(p).is_absolute() or '..' in Path(p).parts for p in paths):
        raise ValueError('SDK overlay must use relative a/ and b/ file paths')
    return paths


def patch_argument(directory, patch, temporary):
    patch_bytes = patch.read_bytes()
    paths = changed_paths(patch_bytes)
    if len(paths) == 1:
        target = directory / next(iter(paths))
        if target.is_file() and b'\r\n' not in target.read_bytes():
            normalized = patch_bytes.replace(b'\r\n', b'\n')
            if normalized != patch_bytes:
                normalized_patch = temporary / 'overlay.patch'
                normalized_patch.write_bytes(normalized)
                return normalized_patch
    return patch


def apply_one(sdk, patch):
    environment = environment_for(sdk)
    apply = ['git', 'apply', '--ignore-whitespace']
    with tempfile.TemporaryDirectory(prefix='apply-sdk-patch-') as temporary:
        argument = str(patch_argument(sdk, patch, Path(temporary)))
        check = subprocess.run(apply + ['--reverse', '--check', argument], cwd=sdk,
                               env=environment, stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL)
        if check.returncode:
            subprocess.run(apply + ['--check', argument], cwd=sdk,
                           env=environment, check=True)
            subprocess.run(apply + [argument], cwd=sdk,
                           env=environment, check=True)


def apply_stack(sdk, patches):
    paths = set().union(*(changed_paths(p.read_bytes()) for p in patches))
    # Later overlays can edit an earlier overlay's context. Test each possible
    # applied prefix by peeling it latest-first, then replay the complete stack.
    # All trials happen on copies; an incompatible or partial state never writes
    # the SDK. Unrelated edits outside patch hunks survive the round trip.
    with tempfile.TemporaryDirectory(prefix='apply-sdk-stack-') as temporary:
        temporary = Path(temporary)
        for applied in range(len(patches), -1, -1):
            trial = temporary / str(applied)
            trial.mkdir()
            for path in paths:
                source = sdk / path
                if source.is_file():
                    target = trial / path
                    target.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copyfile(source, target)
            environment = environment_for(trial)
            sequence = [(p, True) for p in reversed(patches[:applied])]
            sequence += [(p, False) for p in patches]
            for patch, reverse in sequence:
                argument = patch_argument(trial, patch, temporary)
                command = ['git', 'apply', '--ignore-whitespace']
                if reverse:
                    command.append('--reverse')
                result = subprocess.run(command + [str(argument)], cwd=trial,
                                        env=environment, capture_output=True)
                if result.returncode:
                    break
            else:
                for path in sorted(paths):
                    source, target = trial / path, sdk / path
                    if source.is_file():
                        if not target.is_file() or source.read_bytes() != target.read_bytes():
                            target.parent.mkdir(parents=True, exist_ok=True)
                            shutil.copyfile(source, target)
                    elif target.exists():
                        target.unlink()
                return
        raise ValueError('SDK overlay stack is incompatible or partially applied; SDK left unchanged')


def main():
    if len(sys.argv) < 3:
        sys.exit('usage: apply_sdk_patch.py SDK PATCH [PATCH ...]')
    sdk, *patches = [Path(p).resolve() for p in sys.argv[1:]]
    try:
        if len(patches) == 1:
            apply_one(sdk, patches[0])
        else:
            apply_stack(sdk, patches)
    except ValueError as error:
        sys.exit(str(error))


if __name__ == '__main__':
    main()
