"""Loopback HTTPS checks; no DUT, DNS changes or external services."""
import argparse
import hashlib
import http.client
import json
from pathlib import Path
import ssl
import tempfile
import threading
import unittest

import local_ota_server as ota


class LocalOTA(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='bry-ota-test-')
        cls.root = Path(cls.temp.name)
        cls.cert, cls.key = cls.root / 'cert.pem', cls.root / 'key.pem'
        ota.create_certificate(argparse.Namespace(cert=cls.cert, key=cls.key, address='127.0.0.1'))

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def setUp(self):
        self.servers = []
        self.logs = []
        self.device = '020000000001'

    def tearDown(self):
        for server, thread in self.servers:
            server.shutdown()
            server.server_close()
            thread.join()

    def start(self, app=None, broker_ca=None):
        server = ota.Server(('127.0.0.1', 0), ota.make_handler(self.device, 'https://127.0.0.1', app, 'V99', self.logs.append, broker_ca))
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(self.cert, self.key)
        server.socket = context.wrap_socket(server.socket, server_side=True)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        self.servers.append((server, thread))
        self.port = server.server_port

    def request(self, method, path, data=None, headers=None):
        context = ssl.create_default_context(cafile=str(self.cert))
        connection = http.client.HTTPSConnection('127.0.0.1', self.port, context=context, timeout=3)
        body = json.dumps(data) if data is not None else None
        connection.request(method, path, body=body, headers=headers or {})
        response = connection.getresponse()
        raw = response.read()
        result = response.status, raw
        self.assertEqual(int(response.getheader('Content-Length')), len(raw))
        connection.close()
        return result

    def login(self):
        password = ota.stock_password(self.device)
        status, raw = self.request('GET', ota.BASE + f'deviceLogin?deviceId={self.device}&pwd={password}')
        self.assertEqual(status, 200)
        return json.loads(raw)['data']['token']

    def query(self, token, kind='SENSOR3_ESP32', version='V01', device=None):
        return self.request('POST', ota.BASE + 'checkUpgrade-json', {
            'deviceId': device or self.device, 'deviceType': kind, 'firmwareVersion': version}, {'token': token})

    def test_default_no_update_and_private_logs(self):
        self.start()
        token = self.login()
        status, raw = self.query(token)
        self.assertEqual((status, json.loads(raw)['data']), (200, {'haveNewVersion': 0}))
        self.assertEqual(self.request('GET', ota.ARTIFACT)[0], 404)
        self.assertEqual(self.query('wrong')[0], 403)
        self.assertEqual(self.query(token, device='020000000002')[0], 403)
        self.assertEqual(self.request('GET', ota.BASE + f'deviceLogin?deviceId={self.device}&pwd=wrong')[0], 403)
        log = '\n'.join(self.logs)
        for secret in (token, self.device, ota.stock_password(self.device)):
            self.assertNotIn(secret, log)

    def test_full_size_application_download(self):
        app = b'\xe9\x01' + bytes(30) + bytes(range(256)) * 4400
        self.start(app)
        status, received = self.request('GET', ota.ARTIFACT)
        self.assertEqual(status, 200)
        self.assertEqual(hashlib.sha256(received).digest(), hashlib.sha256(app).digest())
        self.assertTrue(any('artifact-start' in line for line in self.logs))

    def test_offer_and_download_digest(self):
        app = bytearray(512);app[0:2]=b'\xe9\x01';app[32:36]=bytes.fromhex('3254cdab');app[288:320]=b'BRY88BBA15G'.ljust(32,b'\0');app[320:352]=b'PHICOMM-DAIKIN-1:full'.ljust(32,b'\0');app=bytes(app)
        source = self.root / 'fixture.bin'
        source.write_bytes(app)
        snapshot = ota.load_app(source)
        self.start(snapshot)
        source.write_bytes(b'changed')
        token = self.login()
        status, raw = self.query(token)
        metadata = json.loads(raw)['data']
        self.assertEqual(status, 200)
        self.assertEqual(metadata['haveNewVersion'], 1)
        self.assertEqual(metadata['firmware']['s3Url'], 'https://127.0.0.1' + ota.ARTIFACT)
        status, downloaded = self.request('GET', ota.ARTIFACT)
        self.assertEqual((status, downloaded), (200, app))
        self.assertEqual(metadata['firmware']['md5'], hashlib.md5(downloaded).hexdigest())
        for kind, version in [('SENSOR3_MCU', 'V01'), ('SENSOR3_GALLERY', 'V01'), ('SENSOR3_ESP32', 'V99')]:
            self.assertEqual(json.loads(self.query(token, kind, version)[1])['data']['haveNewVersion'], 0)
        self.assertEqual(self.request('GET', ota.ARTIFACT, headers={'Range': 'bytes=0-9'})[0], 416)
        self.assertEqual(self.request('POST', ota.BASE + 'checkUpgrade-json', [], {'token': token})[0], 400)

    def test_image_preflight_rejects_wrong_artifacts(self):
        path = self.root / 'invalid.bin'
        for raw in (bytes(64), b'\xe9\x01' + bytes(ota.MAX_APP), b'\xe9\x01' + bytes(10) + b'\x02\0' + bytes(32)):
            path.write_bytes(raw)
            with self.assertRaises(ValueError):
                ota.load_app(path)

    def test_broker_auth_ca_and_target_isolation(self):
        ca = self.cert.read_bytes()
        self.start(broker_ca=ca)
        credentials = {'clientId': 'SENSOR:' + self.device, 'password': ota.stock_password(self.device)}
        status, raw = self.request('POST', '/jwt/getToken', credentials)
        token = json.loads(raw)['data']
        self.assertEqual(status, 200)
        self.assertIsInstance(token, str)
        request = dict(credentials, clientId=self.device, clientType='SENSOR', domainType='DEVICE_EMQ')
        status, raw = self.request('POST', '/gateway/getCaCertificateInfo', request)
        info = json.loads(raw)['data']['downloadInfo']
        self.assertEqual(status, 200)
        self.assertEqual(info['md5'], hashlib.md5(ca).hexdigest())
        self.assertEqual(self.request('GET', '/mqtt/root-ca.pem'), (200, ca))
        for change in ({'clientId': '020000000002'}, {'password': 'wrong'}, {'domainType': 'OTHER'}):
            self.assertEqual(self.request('POST', '/gateway/getCaCertificateInfo', dict(request, **change))[0], 403)
        self.assertEqual(json.loads(self.query(token)[1])['data'], {'haveNewVersion': 0})
        self.assertNotIn(token, '\n'.join(self.logs))

    def test_broker_endpoints_disabled_by_default(self):
        self.start()
        self.assertEqual(self.request('POST', '/jwt/getToken', {})[0], 404)
        self.assertEqual(self.request('GET', '/mqtt/root-ca.pem')[0], 404)


class MigrationPreparation(unittest.TestCase):
    def test_enable_preserves_ca_and_restricts_target(self):
        import importlib.util
        from enable import enable
        spec=importlib.util.spec_from_file_location('migration_init',Path(__file__).with_name('init.py'))
        init=importlib.util.module_from_spec(spec);spec.loader.exec_module(init)
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);lab=root/'lab'
            init.prepare(argparse.Namespace(device='020000000001',version=None,enable_update=False,firmware=None,output=lab,address='127.0.0.1'))
            initial=json.loads((lab/'compose.json').read_text())
            self.assertNotIn('--enable-update',initial['services']['https']['command'])
            ca=(lab/'ca.pem').read_bytes();key=(lab/'server.key').read_bytes()
            image=bytearray(512);image[:2]=b'\xe9\x01';image[32:36]=bytes.fromhex('3254cdab');image[288:320]=b'BRY88BBA15G'.ljust(32,b'\0');image[320:352]=b'PHICOMM-DAIKIN-1:full'.ljust(32,b'\0')
            app=root/'app.bin';app.write_bytes(image)
            before=(lab/'compose.json').read_bytes()
            with self.assertRaises(ValueError):enable(lab,'020000000002',app,'V999')
            self.assertEqual((lab/'compose.json').read_bytes(),before)
            enable(lab,'020000000001',app,'V999')
            after=json.loads((lab/'compose.json').read_text());args=after['services']['https']['command']
            self.assertEqual(after['services']['mqtt'],initial['services']['mqtt'])
            self.assertEqual(args.count('--enable-update'),1)
            self.assertEqual((lab/'ca.pem').read_bytes(),ca);self.assertEqual((lab/'server.key').read_bytes(),key)
            self.assertEqual((lab/'esp32-app.bin').read_bytes(),image)
            self.assertEqual(lab.stat().st_mode&0o777,0o700)
            self.assertEqual((lab/'server.key').stat().st_mode&0o777,0o600)

    def test_trigger_only_esp32_and_valid_version(self):
        from trigger import command
        topic,body=command('020000000001','V999')
        self.assertEqual(topic,'SENSOR/020000000001/mqtt/service/ota')
        self.assertEqual(body['data']['upgrade_info'],[{'device_type':'SENSOR3_ESP32','to_version':'V999'}])
        for value in ('', '../bad', 'x'*32):
            with self.assertRaises(ValueError):command('020000000001',value)

if __name__ == '__main__':
    unittest.main()
