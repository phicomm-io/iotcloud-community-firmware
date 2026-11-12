#!/usr/bin/env python3
"""BRY88BBA15G stock OTA compatibility service; never sends upgrade commands.

Python standard library only. DNS must be configured separately. Firmware is
snapshotted at startup so metadata and served bytes cannot drift during an OTA.
"""
import argparse
import hashlib
import ipaddress
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import re
import secrets
import ssl
import subprocess
import tempfile
from urllib.parse import parse_qs, urlsplit

BASE = '/ota/tenant/VRV/device/'
HOST = 'newlifemulti.daikin-china.com.cn'
ARTIFACT = '/firmware/bry88-esp32-app.bin'
MAX_APP = 0x1E0000


def normalize_device(value):
    value = value.replace(':', '').replace('-', '').upper()
    if not re.fullmatch(r'[0-9A-F]{12}', value):
        raise ValueError('device must be a 12-hex-digit MAC')
    return value


def stock_password(device):
    crc = 0
    for i, byte in enumerate(bytes.fromhex(device)):
        crc ^= (byte + 10 * (i + 1)) & 255
        for _ in range(8):
            if crc & 1:
                crc ^= 0x10811
            crc >>= 1
        crc &= 65535
    return f'{crc:04X}'


def load_app(path):
    raw = path.read_bytes()
    if not 32 <= len(raw) <= MAX_APP:
        raise ValueError('app must be 32..1966080 bytes; whole-flash images are rejected')
    if raw[0] != 0xE9 or not 1 <= raw[1] <= 16 or raw[12:14] != b'\0\0':
        raise ValueError('expected an ESP32 app header (magic E9, ESP32 chip ID 0)')
    if (len(raw)<352 or raw[32:36]!=bytes.fromhex('3254cdab') or
        raw[288:320]!=b'BRY88BBA15G'.ljust(32,b'\0') or
        raw[320:352]!=b'PHICOMM-DAIKIN-1:full'.ljust(32,b'\0')):
        raise ValueError('expected a G3 full application, not G1/G2, a signed envelope or a complete image')
    return raw


def make_handler(device, origin, app=None, version=None, emit=print, broker_ca=None):
    token = secrets.token_urlsafe(24)
    md5 = hashlib.md5(app).hexdigest() if app is not None else None

    class Handler(BaseHTTPRequestHandler):
        protocol_version = 'HTTP/1.0'
        server_version = 'iotcloud-local-ota/1'
        timeout = 15

        def log_message(self, *_):
            pass  # Default logs expose the login query/password.

        def setup(self):
            super().setup()
            self.connection.settimeout(self.timeout)

        def reply(self, status, payload, event, binary=False):
            body = payload if binary else json.dumps(payload, separators=(',', ':')).encode()
            self.send_response(status)
            self.send_header('Content-Type', 'application/octet-stream' if binary else 'application/json')
            self.send_header('Content-Length', str(len(body)))
            self.send_header('Cache-Control', 'no-store')
            self.end_headers()
            if self.command != 'HEAD':
                if binary and event == 'artifact':
                    # Bound TLS records for the stock ESP32's small receive path.
                    # A firmware transfer is allowed more than the JSON timeout.
                    self.connection.settimeout(120)
                    emit(json.dumps({'event': 'artifact-start', 'bytes': len(body)}, separators=(',', ':')))
                    for offset in range(0, len(body), 2048):
                        self.wfile.write(body[offset:offset + 2048])
                else:
                    self.wfile.write(body)
            emit(json.dumps({'event': event, 'status': status, 'bytes': len(body)}, separators=(',', ':')))

        def deny(self, status=403):
            self.reply(status, {'errcode': 1, 'data': {}}, 'rejected')

        def do_GET(self):
            url = urlsplit(self.path)
            if url.path == '/mqtt/root-ca.pem' and broker_ca is not None:
                return self.reply(200, broker_ca, 'broker-ca', binary=True)
            if url.path == BASE + 'deviceLogin':
                query = parse_qs(url.query)
                identity = query.get('deviceId', [''])[0]
                password = query.get('pwd', [''])[0]
                if identity != device or not secrets.compare_digest(password, stock_password(device)):
                    return self.deny()
                return self.reply(200, {'errcode': 0, 'data': {'token': token}}, 'login')
            if url.path == ARTIFACT and app is not None:
                # Stock artifact downloads send no OTA token. Keep this server on LAN.
                if self.headers.get('Range'):
                    return self.deny(416)
                return self.reply(200, app, 'artifact', binary=True)
            self.deny(404)

        def do_HEAD(self):
            if urlsplit(self.path).path == ARTIFACT and app is not None:
                return self.reply(200, app, 'artifact-head', binary=True)
            self.deny(404)

        def do_POST(self):
            path = urlsplit(self.path).path
            broker_path = path in ('/jwt/getToken', '/gateway/getCaCertificateInfo')
            if path != BASE + 'checkUpgrade-json' and not (broker_path and broker_ca is not None):
                return self.deny(404)
            if not broker_path and not secrets.compare_digest(self.headers.get('token', ''), token):
                return self.deny()
            try:
                if self.headers.get('Transfer-Encoding'):
                    return self.deny(400)
                size = int(self.headers.get('Content-Length', '0'))
                if not 0 < size <= 4096:
                    return self.deny(413)
                request = json.loads(self.rfile.read(size))
                if not isinstance(request, dict):
                    return self.deny(400)
                if broker_path:
                    expected_id = 'SENSOR:' + device if path == '/jwt/getToken' else device
                    if request.get('clientId') != expected_id or not secrets.compare_digest(str(request.get('password', '')), stock_password(device)):
                        return self.deny()
                    if path == '/jwt/getToken':
                        # Factory parses data itself as a string, not data.token.
                        # Opaque local broker password; not a vendor-signed JWT.
                        return self.reply(200, {'errcode': 0, 'data': token}, 'broker-login')
                    if request.get('clientType') != 'SENSOR' or request.get('domainType') != 'DEVICE_EMQ':
                        return self.deny()
                    return self.reply(200, {'errcode': 0, 'data': {'downloadInfo': {
                        'resourcesPath': origin + '/mqtt/root-ca.pem',
                        'md5': hashlib.md5(broker_ca).hexdigest()}}}, 'broker-ca-info')
                if request.get('deviceId') != device:
                    return self.deny()
                current = request.get('firmwareVersion')
                if not isinstance(current, str) or not 0 < len(current) <= 128:
                    return self.deny(400)
            except (ValueError, UnicodeError):
                return self.deny(400)
            offer = app is not None and request.get('deviceType') == 'SENSOR3_ESP32' and current != version
            data = {'haveNewVersion': int(offer)}
            if offer:
                data['firmware'] = {'version': version, 's3Url': origin + ARTIFACT, 'md5': md5}
            self.reply(200, {'errcode': 0, 'data': data}, 'offer' if offer else 'no-update')

    return Handler


class Server(ThreadingHTTPServer):
    daemon_threads = True

    def handle_error(self, *_):
        # No raw requests, identities, tokens or passwords in tracebacks/logs.
        print('{"event":"connection-error"}', flush=True)


def create_certificate(args):
    if args.cert.exists() or args.key.exists():
        raise ValueError('certificate/key already exists; choose new paths')
    try:
        ipaddress.ip_address(args.address)
    except ValueError as exc:
        raise ValueError('--address must be the LAN server IP') from exc
    args.cert.parent.mkdir(parents=True, exist_ok=True)
    args.key.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='bry-ota-cert-') as temp:
        conf = Path(temp) / 'openssl.cnf'
        conf.write_text('[req]\nprompt=no\ndistinguished_name=dn\nx509_extensions=ext\n'
                        f'[dn]\nCN={HOST}\n[ext]\nsubjectAltName=DNS:{HOST},DNS:newlifemulti-cdh.daikin-china.com.cn,DNS:newlifemulti-cdh-core.daikin-china.com.cn,IP:{args.address}\n')
        subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-sha256', '-nodes',
                        '-days', '30', '-config', str(conf), '-keyout', str(args.key),
                        '-out', str(args.cert)], check=True, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
    args.key.chmod(0o600)
    print('Certificate created; key permissions 0600.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    cert = sub.add_parser('init-cert')
    cert.add_argument('--address', required=True)
    serve = sub.add_parser('serve')
    for p in (cert, serve):
        p.add_argument('--cert', type=Path, required=True)
        p.add_argument('--key', type=Path, required=True)
    serve.add_argument('--bind', required=True, help='LAN interface IP')
    serve.add_argument('--port', type=int, default=443)
    serve.add_argument('--device', required=True, help='actual device STA MAC')
    serve.add_argument('--origin', required=True, help='device-reachable HTTPS origin, e.g. https://192.168.1.10')
    serve.add_argument('--firmware', type=Path)
    serve.add_argument('--version')
    serve.add_argument('--enable-update', action='store_true', help='explicitly offer the supplied app')
    serve.add_argument('--broker-ca', type=Path, help='enable stock MQTT login/CA endpoints for a local broker')
    args = parser.parse_args()
    try:
        if args.command == 'init-cert':
            create_certificate(args)
            return
        device = normalize_device(args.device)
        origin = urlsplit(args.origin)
        if origin.scheme != 'https' or not origin.hostname or origin.path not in ('', '/') or origin.query or origin.fragment or origin.username or origin.password:
            raise ValueError('--origin must be a plain HTTPS origin without credentials/path')
        _ = origin.port  # Validate the port before advertising it.
        if not args.enable_update and (args.firmware or args.version):
            raise ValueError('firmware/version require --enable-update')
        app = None
        if args.enable_update:
            if not args.firmware or not args.version or not re.fullmatch(r'[A-Za-z0-9._-]{1,64}', args.version):
                raise ValueError('updates require --firmware and a 1..64 character --version')
            app = load_app(args.firmware)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.minimum_version = ssl.TLSVersion.TLSv1_2
        context.load_cert_chain(args.cert, args.key)
        broker_ca = args.broker_ca.read_bytes() if args.broker_ca else None
        if broker_ca is not None:
            if len(broker_ca) > 4096 or b'PRIVATE KEY' in broker_ca:
                raise ValueError('broker CA must be a public PEM certificate of at most 4096 bytes')
            ssl.PEM_cert_to_DER_cert(broker_ca.decode('ascii'))
        server = Server((args.bind, args.port), make_handler(device, args.origin.rstrip('/'), app, args.version,
                        emit=lambda line: print(line, flush=True), broker_ca=broker_ca))
        server.socket = context.wrap_socket(server.socket, server_side=True)
        print(json.dumps({'mode': 'offer-update' if app is not None else 'no-update',
                          'bytes': len(app) if app is not None else 0,
                          'sha256': hashlib.sha256(app).hexdigest() if app is not None else None}), flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass
        finally:
            server.server_close()
    except (ValueError, OSError, subprocess.CalledProcessError) as exc:
        parser.exit(2, f'Error: {exc}\n')


if __name__ == '__main__':
    main()
