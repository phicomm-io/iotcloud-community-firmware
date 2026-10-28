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


def version_range(first, last):
    pattern = r'SV\.\d{2}\.\d{2}\.\d{2}\.\d{2}'
    if not all(re.fullmatch(pattern, value) for value in (first, last)):
        raise ValueError('Version labels must have the form SV.02.00.01.16')
    prefix, start = first.rsplit('.', 1)
    other, end = last.rsplit('.', 1)
    if prefix != other or int(start) > int(end):
        raise ValueError('Use an ascending range with the same SV.xx.xx.xx prefix')
    return [f'{prefix}.{number:02}' for number in range(int(start), int(end) + 1)]


def probe(sock, version):
    """Only send the two handshake headers. An explicit No + EOF is rejection."""
    exchange(sock, b'Kit2 Version Up', b'Okey')
    sock.sendall(version.encode('ascii'))
    reply = bytearray()
    while len(reply) < 4:
        part = sock.recv(4 - len(reply))
        if not part:
            if reply == b'No':
                return False, bytes(reply), 'EOF'
            raise RuntimeError(f'Incomplete version reply: {bytes(reply)!r} / EOF')
        reply.extend(part)
        if reply == b'ToDo':
            return True, bytes(reply), 'complete'
        if not any(value.startswith(reply) for value in (b'No', b'ToDo')):
            raise RuntimeError(f'Unexpected version reply: {bytes(reply)!r}')
    raise RuntimeError(f'Unexpected version reply: {bytes(reply)!r}')


def probe_versions(host, versions):
    print('大金原厂版本标签探测：只握手，不发送固件；不会切换 Wi-Fi。', flush=True)
    print(f'范围：{versions[0]}～{versions[-1]}；间隔 2 秒；遇到 ToDo 或通信异常立即停止。', flush=True)
    rejected, accepted, failure = None, None, None
    try:
        for index, version in enumerate(versions):
            if index:
                time.sleep(2)
            print(f'测试 {version} ...', flush=True)
            try:
                with socket.create_connection((host, 8555), timeout=10) as sock:
                    sock.settimeout(20)
                    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                    ok, reply, ending = probe(sock, version)
            except (OSError, RuntimeError) as error:
                failure = error
                print(f'  通信异常，已停止：{error}', flush=True)
                break
            print(f'  {"accepted" if ok else "rejected"}：{reply!r} / {ending}', flush=True)
            if ok:
                accepted = version
                break
            rejected = version
    finally:
        print('观察结果：', flush=True)
        print(f'  最高明确拒绝标签：{rejected or "未观察到"}', flush=True)
        print(f'  首个接受标签：{accepted or "未观察到"}', flush=True)
        if rejected and accepted:
            print(f'有条件推测当前版本：{rejected}', flush=True)
            print('  仅在“同前缀且只接受更高版本”的假设成立时有效，尚未直接读取确认。', flush=True)
        else:
            print('未形成相邻拒绝 / 接受边界，不能推测当前版本。', flush=True)
        print('边界不能证明已安装版本或固件兼容性。', flush=True)
        print('固件数据发送量：0 字节；不会据此自动执行刷写。', flush=True)
    if failure:
        raise RuntimeError('Version probe stopped on a communication error') from failure


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path, nargs='?')
    parser.add_argument('--sha256', help='Expected SHA256 from the publisher; required with image')
    parser.add_argument('--host', default='192.168.4.1')
    parser.add_argument('--version', help='Explicit stock protocol label higher than the installed version')
    parser.add_argument('--probe-versions', nargs=2, metavar=('FIRST', 'LAST'),
                        help='Probe an inclusive same-prefix range, handshake only; never uploads firmware')
    parser.add_argument('--check-only', action='store_true', help='Validate file without network access')
    args = parser.parse_args()
    if args.probe_versions:
        if args.image or args.sha256 or args.version or args.check_only:
            parser.error('--probe-versions cannot be combined with an image, --sha256, --version or --check-only')
        try:
            versions = version_range(*args.probe_versions)
        except ValueError as error:
            parser.error(str(error))
        probe_versions(args.host, versions)
        return
    if not args.image or not args.sha256:
        parser.error('Upload/check requires image and --sha256; use --probe-versions FIRST LAST for a handshake-only probe')
    if not args.check_only and not args.version:
        parser.error('Upload requires an explicit --version higher than the installed stock label; probe mode never uploads')
    image = args.image.read_bytes()
    digest = hashlib.sha256(image).hexdigest()
    if not re.fullmatch(r'[0-9a-fA-F]{64}', args.sha256) or digest != args.sha256.lower():
        parser.error(f'SHA256 mismatch: actual {digest}')
    if not 24 <= len(image) <= 0x180000 or image[0] != 0xe9 or image[12:14] != b'\0\0':
        parser.error('Expected an ESP32 application image no larger than 1.5 MiB, not factory.bin')
    if args.version and not re.fullmatch(r'SV\.\d{2}\.\d{2}\.\d{2}\.\d{2}', args.version):
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
