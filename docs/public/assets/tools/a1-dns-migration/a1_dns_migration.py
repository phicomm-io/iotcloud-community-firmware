#!/usr/bin/env python3
"""A1 v1.47 LAN migration service, derived from stock firmware (DNS path not device-validated). Python standard library + openssl."""
import argparse
import ipaddress
import re
import struct
import subprocess
import hashlib
import json
import socket
import ssl
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlsplit, parse_qs

def mac_value(value):
    value=value.replace(':','').replace('-','').lower()
    if not re.fullmatch('[0-9a-f]{12}', value):
        raise argparse.ArgumentTypeError('MAC must contain 12 hexadecimal digits')
    return value

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--image',type=Path,required=True)
parser.add_argument('--check-only',action='store_true',help='Validate image without starting services')
parser.add_argument('--sha256',required=True,help='Expected hash from the trusted checksum manifest')
parser.add_argument('--mac',type=mac_value,required=True)
for name in ('device-ip','router-ip','server-ip'):
    parser.add_argument('--'+name,type=ipaddress.IPv4Address,required=True)
parser.add_argument('--state-dir',type=Path,default=Path('a1-migration-state'))
args=parser.parse_args()
IMAGE=args.image.read_bytes()
if hashlib.sha256(IMAGE).hexdigest()!=args.sha256.lower():
    parser.error('SHA-256 does not match; no server started')
# Original A1 OTA slot in the verified stock layout is 0xb5000 bytes.
if not 0x75000+144 <= len(IMAGE) <= 0xb5000:
    parser.error('Native image does not fit the verified factory OTA slot')
length, crc_a, crc_b, magic, stack, abi=struct.unpack_from('<IHHIII',IMAGE,0x75000)
if magic!=0xc89346 or abi!=3 or len(IMAGE)!=0x75000+8+length+16 or crc_a!=crc_b:
    parser.error('Invalid native application header')
product=b'PHICOMM_A1_EMW3080\0'
relative=struct.unpack_from('<I',IMAGE,0x75000+28)[0]-0x08088000
if not 8 <= relative <= length+8-len(product) or IMAGE[0x75000+relative:0x75000+relative+len(product)]!=product:
    parser.error('Image is not a native A1 image; signed/full-flash images are unsupported')
crc=0
for byte in IMAGE[0x75000+8:-16]:
    crc ^= byte << 8
    for _ in range(8):
        crc=((crc << 1) ^ (0x1021 if crc & 0x8000 else 0)) & 0xffff
if crc != crc_a:
    parser.error('Application CRC mismatch')
if hashlib.md5(IMAGE[:-16]).digest()!=IMAGE[-16:]:
    parser.error('Invalid native trailing MD5')
if args.check_only:
    print('Native A1 image validated; no network services started')
    raise SystemExit(0)
ROOT=args.state_dir.resolve()
ROOT.mkdir(parents=True,exist_ok=True)
if not (ROOT/'cert.pem').exists() or not (ROOT/'key.pem').exists():
    subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-keyout',str(ROOT/'key.pem'),
                    '-out',str(ROOT/'cert.pem'),'-days','2','-subj','/CN=aircleaner.phicomm.com'],check=True,
                   stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
ALLOWED={str(args.router_ip),str(args.device_ip),str(args.server_ip),'127.0.0.1'}

def log(kind, **fields):
    print(json.dumps(dict(time=time.time(), kind=kind, **fields)), flush=True)

class Handler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.0'

    def setup(self):
        super().setup()
        self.connection.settimeout(30)

    def do_GET(self):
        parsed = urlsplit(self.path)
        log('http', peer=self.client_address, port=self.server.server_port, path=self.path, headers=dict(self.headers))
        if self.client_address[0] not in ALLOWED:
            self.send_error(403)
            return
        if parsed.path == '/a1-native.bin':
            start, end = 0, len(IMAGE)-1
            status = 200
            r = self.headers.get('Range')
            if r:
                m = re.fullmatch(r'bytes=(\d+)-(\d*)', r)
                if not m:
                    self.send_error(416)
                    return
                start = int(m[1]); end = min(int(m[2]) if m[2] else end, end)
                if start > end:
                    self.send_error(416)
                    return
                status = 206
            self.send_response(status)
            self.send_header('Content-Type', 'application/octet-stream')
            self.send_header('Content-Length', str(end-start+1))
            self.send_header('Accept-Ranges', 'bytes')
            if status == 206:
                self.send_header('Content-Range', f'bytes {start}-{end}/{len(IMAGE)}')
            self.end_headers()
            sent = 0
            try:
                for off in range(start, end+1, 1024):
                    chunk=IMAGE[off:min(off+1024,end+1)]
                    self.wfile.write(chunk); self.wfile.flush(); sent += len(chunk)
                    time.sleep(.01)
                log('download_complete', start=start, end=end, sent=sent)
            except Exception as e:
                log('download_error', sent=sent, error=str(e))
            return
        mac = parse_qs(parsed.query).get('mac', [''])[0].replace(':','').replace('-','').lower()
        if parsed.path == '/aircleanerfirmware/checkUpdate':
            # This stock request has no MAC parameter; access is scoped by the
            # per-device router rules and peer allowlist. Do not wrap in wifiSoftware.
            body = {'url':f'http://{args.server_ip}:29080/a1-native.bin', 'ver':'v1.48', 'md5':hashlib.md5(IMAGE).hexdigest(), 'enforce':True}
        elif parsed.path == '/device/active':
            if mac != args.mac or parse_qs(parsed.query).get('productId') != ['30']:
                self.send_error(403)
                return
            body = {'device_id':args.mac}
        else:
            self.send_error(404)
            return
        data=json.dumps(body,separators=(',',':')).encode()
        self.send_response(200)
        self.send_header('Content-Type','application/json')
        self.send_header('Content-Length',str(len(data)))
        self.send_header('Connection','close')
        self.end_headers()
        self.wfile.write(data)
        log('response', path=parsed.path, body=body)

    def log_message(self, fmt, *args):
        log('access', message=fmt % args)

class TLSServer(ThreadingHTTPServer):
    def get_request(self):
        raw, addr = self.socket.accept()
        raw.settimeout(12)
        try:
            conn = CONTEXT.wrap_socket(raw, server_side=True)
            log('tls', peer=addr, version=conn.version(), cipher=conn.cipher())
            return conn, addr
        except Exception as e:
            log('tls_error', peer=addr, error=str(e)); raw.close(); raise

def tcp_observer(listener):
    with listener:
        while True:
            conn, addr=listener.accept()
            def read(c, a):
                with c:
                    c.settimeout(30)
                    try:
                        while True:
                            b=c.recv(4096)
                            if not b:break
                            log('cloud_tcp', peer=a, hex=b.hex(), text=b.decode(errors='replace'))
                    except Exception as e:log('cloud_tcp_end', peer=a, error=str(e))
            threading.Thread(target=read,args=(conn,addr),daemon=True).start()

CONTEXT=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
CONTEXT.minimum_version=ssl.TLSVersion.TLSv1
CONTEXT.set_ciphers('ALL:@SECLEVEL=0')
CONTEXT.load_cert_chain(ROOT/'cert.pem',ROOT/'key.pem')
http=ThreadingHTTPServer((str(args.server_ip),29080),Handler)
https=TLSServer((str(args.server_ip),29443),Handler)
cloud=socket.create_server((str(args.server_ip),29000))
threading.Thread(target=http.serve_forever,daemon=True).start()
threading.Thread(target=tcp_observer,args=(cloud,),daemon=True).start()
log('ready', size=len(IMAGE), sha256=hashlib.sha256(IMAGE).hexdigest(), md5=hashlib.md5(IMAGE).hexdigest(), mac=args.mac)
https.serve_forever()
