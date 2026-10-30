#!/usr/bin/env python3
"""S30-V1B 2M.1220: local CoAP/HTTPS/TLS-MQTT first installation.

Requires an already networked stock unit, a dedicated TLS MQTT broker, openssl
and paho-mqtt. No serial port, DNS interception or buffer overflow is used.
An HTTP download proves delivery, not successful installation: confirm the new
S30M_V1B_ hotspot and the community version in the browser afterwards.
"""
import argparse
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import ipaddress
import json
from pathlib import Path
import secrets
import socket
import ssl
import struct
import threading
import time
import zlib


def private_json(path):
    path=Path(path)
    if path.stat().st_mode & 0o077:
        raise ValueError(f'Credential file must have mode 600: {path}')
    return json.loads(path.read_text())


def coap_request(payload):
    # CON POST, four-byte token, Uri-Path qlink/netinfo, JSON content format 50.
    return (b'\x44\x02'+secrets.token_bytes(2)+b's30t'+b'\xb5qlink\x07netinfo'
            +b'\x11\x32\xff'+payload)


def validate_rbl(data, expected):
    if hashlib.sha256(data).hexdigest()!=expected.lower():
        raise ValueError('RBL SHA-256 mismatch')
    if not 96<len(data)<=0xAE000:
        raise ValueError('RBL exceeds the stock download partition')
    fields=struct.unpack('<4sHHI16s24s24sIIIII',data[:96])
    if (fields[0]!=b'RBL\0' or fields[1]!=0x102 or fields[4].split(b'\0',1)[0]!=b'app'
        or fields[10]!=len(data)-96 or fields[11]!=zlib.crc32(data[:92])
        or fields[7]!=zlib.crc32(data[96:])):
        raise ValueError('Invalid application RBL header or payload CRC')


class Server(ThreadingHTTPServer):
    daemon_threads=True
    allow_reuse_address=True


def gateway_handler(device, reply):
    class Gateway(BaseHTTPRequestHandler):
        protocol_version='HTTP/1.1'

        def do_POST(self):
            if self.client_address[0]!=device:
                self.send_error(403);return
            try:length=int(self.headers.get('Content-Length','0'))
            except ValueError:self.send_error(400);return
            if not 0<=length<=16384:
                self.send_error(413);return
            self.connection.settimeout(12)
            if len(self.rfile.read(length))!=length:
                self.close_connection=True;return
            self.send_response(200)
            self.send_header('Content-Type','application/json')
            self.send_header('Content-Length',str(len(reply)))
            self.send_header('Connection','keep-alive')
            self.end_headers();self.wfile.write(reply);self.wfile.flush()

        def log_message(self,*args):pass
    return Gateway


def artifact_handler(device, data, downloaded):
    class Artifact(BaseHTTPRequestHandler):
        def do_GET(self):
            if self.client_address[0]!=device or self.path!='/s30.rbl':
                self.send_error(404);return
            self.send_response(200)
            self.send_header('Content-Type','application/octet-stream')
            self.send_header('Content-Length',str(len(data)))
            self.end_headers();self.wfile.write(data);self.wfile.flush()
            downloaded.set()

        def log_message(self,*args):pass
    return Artifact


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--device',required=True,help='Already networked stock device IPv4 address')
    p.add_argument('--host',required=True,help='This computer IPv4 address reachable by the device')
    p.add_argument('--device-id',required=True,help='This unit factory MAC, 12 hexadecimal digits')
    p.add_argument('--wifi-file',type=Path,required=True,help='Private JSON: ssid/password')
    p.add_argument('--mqtt-file',type=Path,required=True,help='Private JSON: host/port/username/password of dedicated TLS broker')
    p.add_argument('--mqtt-ca',type=Path,required=True)
    p.add_argument('--cert',type=Path,required=True,help='Local HTTPS certificate')
    p.add_argument('--key',type=Path,required=True,help='Local HTTPS private key')
    p.add_argument('--rbl',type=Path,required=True)
    p.add_argument('--sha256',required=True)
    p.add_argument('--https-port',type=int,default=15683)
    p.add_argument('--http-port',type=int,default=8080)
    p.add_argument('--timeout',type=int,default=240)
    args=p.parse_args()
    device=str(ipaddress.IPv4Address(args.device));host=str(ipaddress.IPv4Address(args.host))
    identity=bytes.fromhex(args.device_id)
    if len(identity)!=6 or identity[0]&1 or not any(identity[3:]):
        p.error('Use the valid factory MAC of this unit')
    raw=args.rbl.read_bytes();validate_rbl(raw,args.sha256)
    wifi=private_json(args.wifi_file);broker=private_json(args.mqtt_file)
    if not isinstance(wifi['ssid'],str) or not 1<=len(wifi['ssid'].encode())<=32:
        p.error('Invalid Wi-Fi SSID')
    if not isinstance(wifi['password'],str) or len(wifi['password'].encode())>63:
        p.error('Invalid Wi-Fi password')
    broker_host=str(ipaddress.IPv4Address(broker['host']));port=int(broker['port'])
    if not 1<=port<=65535 or not all(isinstance(broker[k],str) and 0<len(broker[k].encode())<=31 for k in ['username','password']):
        p.error('Use short dedicated MQTT credentials (1–31 bytes)')
    mqtt_url=f'ssl://{broker_host}:{port}'
    payload=json.dumps({'SSID':wifi['ssid'],'password':wifi['password'],
                        'CGW':{'gwAddress2':f'https://{host}:{args.https_port}','user_key':secrets.token_hex(4)}},separators=(',',':')).encode()
    value={'respCode':1,'respCont':'SUCCESS','deviceId':identity.hex(),
           'mqttUrl':mqtt_url,'mqttUser':broker['username'],'mqttPassword':broker['password'],
           'mqttClientId':'s30-install','mqttKeepalive':60,'timestamp':int(time.time()*1000)}
    value.update({key:secrets.token_hex(4) for key in ['andlinkToken','gwToken','deviceToken','dmToken']})
    context=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.set_ciphers('DEFAULT:@SECLEVEL=0')
    context.load_cert_chain(args.cert,args.key)
    gateway=Server((host,args.https_port),gateway_handler(device,json.dumps(value,separators=(',',':')).encode()))
    gateway.socket=context.wrap_socket(gateway.socket,server_side=True)
    downloaded=threading.Event()
    http=Server((host,args.http_port),artifact_handler(device,raw,downloaded))
    from paho.mqtt import client as mqtt
    client=mqtt.Client(client_id='iotcloud-s30-flash',protocol=mqtt.MQTTv311)
    client.username_pw_set(broker['username'],broker['password'])
    client.tls_set(ca_certs=str(args.mqtt_ca))
    topic='/device/s30-install/downward'
    connected=threading.Event()
    def on_connect(client,userdata,flags,rc):
        if rc==0:connected.set()
    client.on_connect=on_connect
    try:
        for server in [gateway,http]:threading.Thread(target=server.serve_forever,daemon=True).start()
        client.connect(broker_host,port,60);client.loop_start()
        if not connected.wait(10):raise TimeoutError('Test TLS broker connection failed')
        command=json.dumps({'function':'file','fileMode':0,'fileUrl':f'http://{host}:{args.http_port}/s30.rbl'},separators=(',',':')).encode()+b'\0'
        client.publish(topic,command,qos=1,retain=True).wait_for_publish(timeout=10)
        with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as udp:
            udp.settimeout(4);udp.sendto(coap_request(payload),(device,5683))
            try:
                response,peer=udp.recvfrom(2048)
                if peer[0]!=device or len(response)<4 or response[1]>>5!=2:
                    raise ValueError('Stock provisioning rejected')
            except TimeoutError:
                print('No CoAP reply; waiting for the stock network download.',flush=True)
        print('Waiting for the stock device to download the RBL…',flush=True)
        if not downloaded.wait(args.timeout):raise TimeoutError('No complete device HTTP download; installation unconfirmed')
        print(f'RBL HTTP delivery completed: {len(raw)} bytes, SHA-256 {args.sha256.lower()}.',flush=True)
        print('Confirm S30M_V1B_XXXXXX, configure Wi-Fi at http://192.168.0.1 and check the community version. Delivery alone is not installation proof.',flush=True)
    finally:
        if connected.is_set():
            try:client.publish(topic,b'',qos=1,retain=True).wait_for_publish(timeout=5)
            except Exception:pass
        client.disconnect();client.loop_stop()
        for server in [http,gateway]:server.shutdown();server.server_close()


if __name__=='__main__':main()
