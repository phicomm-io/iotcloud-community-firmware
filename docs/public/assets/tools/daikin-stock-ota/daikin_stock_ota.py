#!/usr/bin/env python3
"""Upload an ESP32 application through the Daikin SensorKit2 TCP OTA service.

Python 3 standard library only. Tested stock: SV.02.00.01.15, BRY88AB151K.
"""
import argparse
import hashlib
from pathlib import Path
import re
import socket
import struct
import time


def receive(sock, count):
    data = bytearray()
    while len(data) < count:
        part = sock.recv(count - len(data))
        if not part:
            raise RuntimeError('Device closed the connection before the full reply')
        data.extend(part)
    return bytes(data)


def exchange(sock, message, expected):
    sock.sendall(message)
    reply = receive(sock, len(expected))
    if reply != expected:
        raise RuntimeError(f'Unexpected response to {message!r}: {reply!r}')
    print(f'{message.decode()} -> {reply.decode()}', flush=True)


def upload(sock, image, version):
    exchange(sock, b'Kit2 Version Up', b'Okey')
    exchange(sock, version.encode('ascii'), b'ToDo')
    acknowledged = 0
    last_percent = -10
    for offset in range(0, len(image), 1024):
        chunk = image[offset:offset + 1024]
        if chunk == b'End':
            raise RuntimeError('A three-byte End data block cannot be sent by this protocol')
        sock.sendall(chunk)
        block_ack = 0
        while block_ack < len(chunk):
            length, = struct.unpack('<I', receive(sock, 4))
            if not 0 < length <= len(chunk) - block_ack:
                raise RuntimeError(f'Invalid ACK {length} at offset {offset}')
            block_ack += length
        acknowledged += block_ack
        percent = acknowledged * 100 // len(image)
        if percent >= last_percent + 10 or acknowledged == len(image):
            print(f'ACK {acknowledged}/{len(image)} ({percent}%)', flush=True)
            last_percent = percent
    total = sum(image) & 0xffffffff
    while total > 255:
        total = (total & 255) + (total >> 8)
    checksum = (~total) & 255
    sock.sendall(b'End')
    # There is no End ACK. Keep the checksum out of the End TCP read.
    time.sleep(3)
    sock.sendall(bytes([checksum]))
    reply = receive(sock, 2)
    print(f'Final response: {reply!r}', flush=True)
    if reply != b'OK':
        raise RuntimeError('OTA was rejected; do not assume the new image booted')
    print('Transfer accepted. Verify the device version after reboot.', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path)
    parser.add_argument('--sha256', required=True, help='Expected SHA256 from the publisher')
    parser.add_argument('--host', default='192.168.4.1')
    parser.add_argument('--version', default='SV.02.00.01.16', help='Stock protocol version label')
    parser.add_argument('--check-only', action='store_true', help='Validate file without network access')
    args = parser.parse_args()
    image = args.image.read_bytes()
    digest = hashlib.sha256(image).hexdigest()
    if not re.fullmatch(r'[0-9a-fA-F]{64}', args.sha256) or digest != args.sha256.lower():
        parser.error(f'SHA256 mismatch: actual {digest}')
    if not 24 <= len(image) <= 0x180000 or image[0] != 0xe9 or image[12:14] != b'\0\0':
        parser.error('Expected an ESP32 application image no larger than 1.5 MiB, not factory.bin')
    if not re.fullmatch(r'SV\.\d{2}\.\d{2}\.\d{2}\.\d{2}', args.version):
        parser.error('Version label must have the form SV.02.00.01.16')
    if image[-3:] == b'End' and len(image) % 1024 == 3:
        parser.error('Unsupported final three-byte End data block')
    print(f'Image: {len(image)} bytes; SHA256 {digest}', flush=True)
    if args.check_only:
        return
    print(f'Target: {args.host}:8555. Keep power connected throughout.', flush=True)
    with socket.create_connection((args.host, 8555), timeout=10) as sock:
        sock.settimeout(20)
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        upload(sock, image, args.version)


if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError) as error:
        raise SystemExit(f'ERROR: {error}')
