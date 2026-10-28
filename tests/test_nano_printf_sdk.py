"""The printf profile changes only one validated linker selection, preserving SDK bytes."""
from pathlib import Path
import importlib.util
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('nano_printf',ROOT/'tools/install_nano_printf_sdk.py')
profile=importlib.util.module_from_spec(spec);spec.loader.exec_module(profile)


class NanoPrintfSDK(unittest.TestCase):
    def test_atomic_selection_idempotence_and_crlf_preservation(self):
        with tempfile.TemporaryDirectory() as td:
            core=Path(td)/'MiCO/core/core.mk';core.parent.mkdir(parents=True)
            prefix=b'# SDK core\r\n$(NAME)_SOURCES += moc_main.c\r\n';suffix=b'\r\nGLOBAL_CFLAGS += -mthumb\r\n'
            before=prefix+profile.OLD+suffix;core.write_bytes(before)
            profile.install(td);self.assertEqual(core.read_bytes(),prefix+profile.NEW+suffix)
            stamp=core.stat().st_mtime_ns;profile.install(td);self.assertEqual(stamp,core.stat().st_mtime_ns)
            for invalid in (b'unknown SDK layout',profile.OLD+b'\n'+profile.OLD,profile.OLD+b'\n'+profile.NEW,profile.NEW+b'\n'+profile.NEW):
                core.write_bytes(invalid)
                with self.assertRaises(RuntimeError):profile.install(td)
                self.assertEqual(core.read_bytes(),invalid)

    def test_model_build_hooks_and_tc1_existing_profile(self):
        for model in ('A1','M1'):
            text=(ROOT/model/'build.sh').read_text()
            self.assertEqual(text.count('tools/install_nano_printf_sdk.py'),1)
            self.assertLess(text.index('tools/prepare_sdk.py" "$sdk_dir"'),text.index('tools/install_nano_printf_sdk.py'))
        self.assertIn('tools/install_tc1_native_sdk.py',(ROOT/'TC1/build.sh').read_text())
        self.assertNotIn('tools/install_nano_printf_sdk.py',(ROOT/'TC1/build.sh').read_text())


if __name__=='__main__':unittest.main()
