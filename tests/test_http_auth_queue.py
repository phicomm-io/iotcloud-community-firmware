"""Production A1/M1/TC1 routes retain auth across admission/body waits."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / 'tests/http_core'


class HTTPAuthQueue(unittest.TestCase):
    def test_product_auth_contexts(self):
        for model in ('A1', 'M1', 'TC1'):
            with self.subTest(model=model), tempfile.TemporaryDirectory() as temporary:
                temp = Path(temporary)
                # Reuse the existing platform/service fixtures, replacing their
                # fake bearer boundary with the real core and adapter.
                fixture = (FIXTURE / 'callbacks_mp.c').read_text()
                begin = fixture.index('/* Auth core and token routes')
                end = fixture.index('static bool ota_job_active', begin)
                fixture = fixture[:begin] + (FIXTURE / 'auth_queue.inc').read_text() + fixture[end:]
                source = (ROOT / (model + '/http.c' if model != 'TC1' else 'TC1/http_server/app_httpd.c')).read_text()
                route = source[source.index('#define HTTP_PENDING_CLIENTS' if model != 'TC1' else 'static ph_http_product product;'):]
                route = route.replace('../../common/', 'common/').replace('../common/', 'common/')
                if model != 'TC1':
                    name = model.lower()
                    stubs = (FIXTURE / 'mico_mp.inc').read_text().replace('MODEL', model).replace('app_', name + '_')
                    stubs += (FIXTURE / ('mcu_mp.inc' if model == 'M1' else 'light_mp.inc')).read_text()
                    stubs += f'\n#define {model}_WEB_ETAG "\\\"page\\\""\n#define {model}_FAVICON_ETAG "\\\"icon\\\""\nstatic const unsigned char {name}_web_data[]={{31,139,1,2}},{name}_favicon_data[]={{1,2}};\n'
                else:
                    stubs = (FIXTURE / 'tc1_mp.inc').read_text()
                helpers = (FIXTURE / 'callbacks_mp_main.inc').read_text().split('int main(')[0]
                main = (FIXTURE / 'auth_queue_main.inc').read_text().replace('MODEL_START', 'AppHttpdStart' if model == 'TC1' else model.lower() + '_http_start')
                probe = temp / 'probe.c'
                probe.write_text(fixture + stubs + route + helpers + main)
                binary = temp / 'auth-queue'
                sources = ('common/httpd/product.c', 'common/mqtt/diagnostics.c', 'common/httpd/parser.c',
                           'common/httpd/core.c', 'common/typed_wire.c', 'common/wire_buffer.c', 'common/local_auth.c')
                command = [os.environ.get('CC', 'cc'), '-std=gnu99', '-g', '-fsanitize=address,undefined',
                           '-fno-omit-frame-pointer', '-no-pie', '-I' + str(ROOT), '-I' + str(ROOT / 'tests/stubs'),
                           str(probe), *[str(ROOT / path) for path in sources], '-lm', '-o', str(binary)]
                result = subprocess.run(command, capture_output=True, text=True, timeout=120)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                result = subprocess.run([binary], capture_output=True, text=True, timeout=30)
                print(model, result.stdout, flush=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
