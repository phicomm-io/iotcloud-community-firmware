"""Production HTTP/auth/Hub queue adapter with real localhost UDP v4 sockets."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest
from DAIKIN.tests.arduinojson import codec_source

ROOT = Path(__file__).resolve().parents[2]


class HubHttpCommand(unittest.TestCase):
    def test_common_auth_http_queue_udp_correlated_result(self):
        with tempfile.TemporaryDirectory(prefix='hub-http-command-') as directory:
            build = Path(directory)
            http = (ROOT/'DAIKIN/src/http.inc').read_text()
            lan = (ROOT/'DAIKIN/src/hub_lan.inc').read_text()
            hub = (ROOT/'DAIKIN/src/hub.inc').read_text()
            main = (ROOT/'DAIKIN/src/main.cpp').read_text()

            def section(text, start, end):
                begin = text.index(start)
                return text[begin:text.index(end, begin)]

            parts = {
                'http-auth.inc': http[:http.index('// HTTP handlers')],
                'http-globals.inc': section(http, 'static portMUX_TYPE webMux', 'static esp_err_t rootHandler'),
                'http-api.inc': section(http, 'static esp_err_t hubApiHandler(', 'static void registerHubHttp('),
                'lan-gates.inc': section(lan, 'static bool hubLanRegistered(', 'static void hubLanStatus(')
                    + section(lan, 'static bool hubLanRouteConfirmed(const iotcloud::HubLanDiscovery::Peer &peer){', 'static void hubLanReset('),
                'lan-start.inc': section(lan, 'static void hubLanObserveDiscovery(', '// A transport failure is a correlated result,'),
                'lan-failed.inc': section(lan, 'static void hubLanFailed(', 'static bool hubLanSend('),
                'lan-authorize.inc': section(lan, '  hubLanClient.authorize=[]', '  hubLanClient.send=[]'),
                'http-command.inc': lan[lan.index('// Return true for this route,'):],
                'hub-api.inc': section(hub, 'static void hubApiExecute()', 'static void hubLocalPublish('),
                'http-dispatch.inc': section(main, '  if(webPending){', '  if(mqttQueued)'),
            }
            for name, text in parts.items():
                (build/name).write_text(text)
            (build/'wire.c').write_text('#include "lan_wire_v4.inc"\n')
            flags = ['-g', '-O1', '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie']
            objects = []
            for index, source in enumerate([ROOT/'common/typed_wire.c', ROOT/'common/wire_buffer.c',
                                            ROOT/'common/local_auth.c', build/'wire.c']):
                obj = build/(str(index)+'.o')
                subprocess.run(['cc', '-std=c11', *flags, '-I'+str(ROOT/'common'), '-c', str(source), '-o', str(obj)], check=True)
                objects.append(str(obj))
            binary = build/'test'
            subprocess.run(['c++', '-std=c++11', *flags, '-no-pie', '-Wall', '-Wextra', '-Werror',
                            '-Wno-unused-function', '-Wno-misleading-indentation', '-DIOTCLOUD_HUB=1',
                            '-I'+str(build), '-I'+str(ROOT/'common'), '-I'+str(ROOT/'DAIKIN/include'),
                            '-I'+str(codec_source()), str(ROOT/'DAIKIN/tests/hub/http_command.cpp'),
                            *objects, '-lcrypto', '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=30,
                           env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1', 'UBSAN_OPTIONS': 'halt_on_error=1'})


if __name__ == '__main__':
    unittest.main()
