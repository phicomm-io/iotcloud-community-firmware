"""A1 SDK profile selection and packaged target identity."""
import hashlib
import json
import os
from pathlib import Path
import runpy
import shutil
import subprocess
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

from tools.check_version import project_version


ROOT = Path(__file__).resolve().parents[1]
BASE_TARGET = 'A1@MK3080B@moc'
VARIANTS = ('full', 'no-frontend-resources', 'fresh-air',
            'fresh-air-no-frontend-resources')


class A1BuildProfile(unittest.TestCase):
    def test_build_selects_release_by_default_and_keeps_explicit_log_profile(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'A1').mkdir()
            script = root / 'A1/build.sh'
            shutil.copyfile(ROOT / 'A1/build.sh', script)
            binary = root / 'bin'
            binary.mkdir()
            fake = binary / 'python3'
            fake.write_text('#!' + sys.executable + '\n' + r'''
import json, os, sys
if sys.argv[1].endswith('/prepare_sdk.py'):
    if '--commit' in sys.argv:
        print('fixture-sdk')
    else:
        keys = ('PHICOMM_A1_BUILD_TYPE', 'PHICOMM_A1_BUILD_TARGET',
                'A1_FRESH_AIR', 'PHICOMM_FULL_RESOURCES')
        with open(os.environ['PROFILE_LOG'], 'w') as log:
            json.dump({key: os.environ.get(key) for key in keys}, log)
        sys.exit(87)
''')
            fake.chmod(0o755)
            for selected in (None, 'release', 'release_log'):
                for variant in VARIANTS:
                    with self.subTest(selected=selected, variant=variant):
                        log = root / 'selection.json'
                        environment = dict(os.environ, PATH=str(binary) + os.pathsep + os.environ['PATH'],
                                           PROFILE_LOG=str(log), PHICOMM_FIRMWARE_VARIANT=variant,
                                           A1_TOOLS_DIR=str(root / 'shared-tools'))
                        environment.pop('PHICOMM_A1_BUILD_TYPE', None)
                        environment.pop('PHICOMM_A1_BUILD_TARGET', None)
                        if selected is not None:
                            environment['PHICOMM_A1_BUILD_TYPE'] = selected
                        result = subprocess.run(['/bin/bash', str(script)], env=environment,
                                                text=True, capture_output=True)
                        self.assertEqual(result.returncode, 87, result.stderr)
                        actual = json.loads(log.read_text())
                        profile = selected or 'release'
                        self.assertEqual(actual['PHICOMM_A1_BUILD_TYPE'], profile)
                        self.assertEqual(actual['PHICOMM_A1_BUILD_TARGET'], BASE_TARGET +
                                         ('@release' if profile == 'release' else ''))
                        self.assertEqual(actual['A1_FRESH_AIR'], str(int(variant.startswith('fresh-air'))))
                        self.assertEqual(actual['PHICOMM_FULL_RESOURCES'], str(int('no-frontend' not in variant)))
            log.unlink()
            environment['PHICOMM_A1_BUILD_TYPE'] = 'debug'
            result = subprocess.run(['/bin/bash', str(script)], env=environment,
                                    text=True, capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('Unsupported A1 SDK build type: debug', result.stderr)
            self.assertFalse(log.exists())

    def package(self, source, output, environment):
        checker = types.ModuleType('check_image')
        checker.check = lambda path: {'sha256': hashlib.sha256(Path(path).read_bytes()).hexdigest()}
        script = ROOT / 'A1/tools/package.py'
        clean = dict(os.environ)
        for key in ('PHICOMM_A1_BUILD_TARGET', 'PHICOMM_A1_BUILD_TYPE', 'PHICOMM_FIRMWARE_VARIANT'):
            clean.pop(key, None)
        clean.update(environment)
        with patch.dict(os.environ, clean, clear=True), patch.dict(sys.modules, {'check_image': checker}), \
                patch.object(sys, 'argv', [str(script), str(source), str(output), 'fixture-sdk']):
            runpy.run_path(str(script), run_name='__main__')

    def test_package_records_selected_target_and_profile_for_every_variant(self):
        version = project_version().encode()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / 'source'
            source.mkdir()
            for profile in ('release', 'release_log'):
                prefix = BASE_TARGET + ('@release' if profile == 'release' else '')
                for suffix in ('.usr.bin', '.ota.bin', '.all.bin', '.elf', '.map'):
                    (source / (prefix + suffix)).write_bytes(prefix.encode() + suffix.encode() + b'\0' + version + b'\0')
                for variant in VARIANTS:
                    with self.subTest(profile=profile, variant=variant):
                        output = root / (profile + '-' + variant)
                        self.package(source, output, {'PHICOMM_A1_BUILD_TARGET': prefix,
                                     'PHICOMM_A1_BUILD_TYPE': profile, 'PHICOMM_FIRMWARE_VARIANT': variant})
                        manifest = json.loads((output / 'manifest.json').read_text())
                        self.assertEqual(manifest['target'], prefix)
                        self.assertEqual(manifest['sdk_build_type'], profile)
                        self.assertEqual(manifest['firmware_variant'], variant)
                        self.assertEqual(len(manifest['files']), 5)
                        for name, digest in manifest['files'].items():
                            data = (output / name).read_bytes()
                            self.assertTrue(data.startswith(prefix.encode()))
                            self.assertEqual(hashlib.sha256(data).hexdigest(), digest)
            # Direct packaging of an existing unsuffixed SDK build keeps its
            # original release_log identity when no build environment is set.
            output = root / 'standalone'
            self.package(source, output, {})
            self.assertEqual(json.loads((output / 'manifest.json').read_text())['sdk_build_type'], 'release_log')

    def test_package_rejects_unknown_or_mismatched_profiles_before_output_changes(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output = root / 'output'
            output.mkdir()
            signed = output / 'phicomm_a1.ota.bin.signed'
            signed.write_bytes(b'existing signed image')
            for environment in (
                {'PHICOMM_A1_BUILD_TARGET': 'A1@OTHER@moc@release'},
                {'PHICOMM_A1_BUILD_TARGET': BASE_TARGET, 'PHICOMM_A1_BUILD_TYPE': 'release'},
                {'PHICOMM_A1_BUILD_TARGET': BASE_TARGET + '@release', 'PHICOMM_A1_BUILD_TYPE': 'release_log'},
                {'PHICOMM_A1_BUILD_TYPE': 'debug'},
            ):
                with self.subTest(environment=environment), self.assertRaises(ValueError):
                    self.package(root / 'missing', output, environment)
                self.assertEqual(signed.read_bytes(), b'existing signed image')


if __name__ == '__main__':
    unittest.main()
