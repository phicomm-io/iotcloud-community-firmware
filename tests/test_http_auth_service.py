"""HTTP contexts survive the real blocking service gate and map revoke to 401."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class HTTPAuthService(unittest.TestCase):
    def test_revoke_while_waiting_for_service_gate(self):
        with tempfile.TemporaryDirectory(prefix='http-auth-service-') as directory:
            temp = Path(directory)
            (temp / 'generated').mkdir()
            (temp / 'generated/build_ota.h').write_text('')
            sources = ('tests/http_core/auth_service.c', 'common/service.c',
                       'tests/service_mp/hooks.c', 'common/httpd/product.c',
                       'common/local_auth.c', 'common/typed_wire.c', 'common/wire_buffer.c')
            binary = temp / 'auth-service'
            command = shlex.split(os.environ.get('CC', 'cc')) + [
                '-std=gnu99', '-DMOC=1', '-Wall', '-Wextra', '-Werror',
                '-Wno-misleading-indentation', '-Wno-deprecated-declarations',
                '-Wno-unused-function', '-Wno-unused-variable', '-g',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie',
                '-pthread', '-I' + str(temp), '-I' + str(ROOT / 'tests/service_mp'),
                '-I' + str(ROOT), *[str(ROOT / source) for source in sources],
                '-Wl,--wrap=malloc', '-Wl,--wrap=free', '-Wl,--wrap=sem_wait',
                '-Wl,--wrap=sem_trywait', '-lcrypto', '-lm', '-o', str(binary)]
            result = subprocess.run(command, capture_output=True, text=True, timeout=45)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([binary], capture_output=True, text=True, timeout=30)
            print(result.stdout, flush=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
