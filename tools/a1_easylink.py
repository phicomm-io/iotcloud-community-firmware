#!/usr/bin/env python3
"""Local EasyLink V2 provisioning sender for the backed-up A1 stock firmware.

Python 3.9+, standard library only. See docs/a1-easylink.md for protocol
evidence, network requirements, and the limits of offline verification.
"""

import argparse
from dataclasses import dataclass, field
import getpass
import ipaddress
import math
import socket
import sys
import time


GUIDE_ADDRESS = "239.118.0.0"
DEFAULT_PORT = 8001


@dataclass(frozen=True)
class Datagram:
    # The destination encodes credentials; keep it out of logs and repr().
    address: str = field(repr=False)
    length: int


def interface_ipv4(value):
    """Require a concrete unicast IPv4 address of the sending Wi-Fi adapter."""
    try:
        address = ipaddress.IPv4Address(value)
    except ipaddress.AddressValueError:
        raise ValueError("请填写 Wi-Fi 网卡的 IPv4 地址，例如 192.168.1.23") from None
    if (address.is_unspecified or address.is_multicast or address.is_loopback
            or address.is_reserved or int(address) == 0xFFFFFFFF):
        raise ValueError("需要实际 Wi-Fi 网卡的单播 IPv4 地址")
    return address


def credentials(ssid, password):
    """Validate UTF-8 octet lengths, which are the lengths used by the device."""
    ssid_bytes, key = ssid.encode("utf-8"), password.encode("utf-8")
    if not 1 <= len(ssid_bytes) <= 32 or b"\0" in ssid_bytes:
        raise ValueError("Wi-Fi 名称必须为 1–32 个 UTF-8 字节，且不能包含 NUL")
    if b"\0" in key:
        raise ValueError("Wi-Fi 密码不能包含 NUL")
    if not key or 8 <= len(key) <= 63:
        return ssid_bytes, key
    if len(key) == 64 and all(c in b"0123456789abcdefABCDEF" for c in key):
        return ssid_bytes, key
    raise ValueError("密码须为 8–63 个 UTF-8 字节、64 位十六进制 PSK；开放网络用 --open")


def encode(ssid, password, interface_ip):
    """Return A1's padded V2 record (not EasyLink Plus or ESPTouch).

    The application requires extra data ending in '#' + a 4-byte identifier.
    That form also selects DHCP. Use the sender's IPv4 octets as identifier.
    The octet after extra-data length is reserved; this receiver ignores it.
    """
    name, key = credentials(ssid, password)
    extra = b"#" + interface_ipv4(interface_ip).packed
    record = bytes((len(name), len(key))) + name + key
    record += b"\0" * (len(record) % 2)
    record += bytes((len(extra), 0)) + extra
    record += b"\0" * (len(record) % 2)
    return record


def data_packets(record):
    """Two data bytes in each multicast MAC; UDP length = 20 + pair index."""
    if not record or len(record) % 2 or len(record) > 256:
        raise ValueError("V2 记录需要非空、偶数长度，且最多 256 字节")
    return tuple(Datagram(f"239.126.{record[i]}.{record[i + 1]}", 20 + i // 2)
                 for i in range(0, len(record), 2))


def packet_cycle(packets, guide_count=20, repeat=3):
    """Revisit the guide and repeat each pair so channel scanning/loss can recover."""
    yield from (Datagram(GUIDE_ADDRESS, 20) for _ in range(guide_count))
    for packet in packets:
        yield from (packet for _ in range(repeat))


def send(packets, interface_ip, duration=50.0, interval=0.01, port=DEFAULT_PORT,
         guide_count=20, repeat=3, progress=None, cancel=None):
    """Send bounded UDP multicast traffic. Counts are not a device acknowledgement."""
    address = str(interface_ipv4(interface_ip))
    if not packets:
        raise ValueError("没有待发送的数据包")
    if not math.isfinite(duration) or not 1 <= duration <= 60:
        raise ValueError("发送时长必须在 1–60 秒之间")
    if not math.isfinite(interval) or not 0.001 <= interval <= 0.2:
        raise ValueError("发送间隔必须在 1–200 毫秒之间")
    if not 1 <= port <= 65535 or guide_count < 1 or repeat < 1:
        raise ValueError("端口、引导包数和重复次数必须为有效正数")
    count = cycles = 0
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP) as sock:
        # Bind and select the adapter explicitly: don't accidentally use Ethernet/VPN.
        sock.bind((address, 0))
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF,
                        socket.inet_aton(address))
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 1)
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_LOOP, 0)
        sock.settimeout(0.5)
        start = time.monotonic()
        deadline, report_at = start + duration, start + 5
        while time.monotonic() < deadline:
            if cancel is not None and cancel.is_set():
                break
            complete = True
            for packet in packet_cycle(packets, guide_count, repeat):
                if (time.monotonic() >= deadline
                        or (cancel is not None and cancel.is_set())):
                    complete = False
                    break
                sock.sendto(bytes(packet.length), (packet.address, port))
                count += 1
                now = time.monotonic()
                if progress and now >= report_at:
                    progress(count, cycles, now - start)
                    report_at = now + 5
                delay = min(interval, max(0, deadline - now))
                if cancel is None:
                    time.sleep(delay)
                else:
                    cancel.wait(delay)
            if complete:
                cycles += 1
    return count, cycles


def make_parser():
    parser = argparse.ArgumentParser(
        description="A1 EasyLink V2 UDP 实验发送器（成功方案仅限 Linux mac80211 注入）")
    parser.add_argument("--ssid", help="目标 2.4 GHz Wi-Fi 名称；省略则交互输入")
    parser.add_argument("--interface-ip", required=True,
                        help="电脑连接目标 Wi-Fi 后，该 Wi-Fi 网卡的 IPv4 地址")
    parser.add_argument("--open", action="store_true", help="目标为无密码的开放网络")
    parser.add_argument("--duration", type=float, default=50, help="发送秒数，1–60，默认 50")
    parser.add_argument("--interval-ms", type=float, default=10, help="包间隔毫秒，默认 10")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help="UDP 目的端口，默认 8001")
    parser.add_argument("--dry-run", action="store_true", help="只检查编码，不打开网络套接字")
    return parser


def main(argv=None):
    parser = make_parser()
    args = parser.parse_args(argv)
    try:
        address = str(interface_ipv4(args.interface_ip))
        if not math.isfinite(args.duration) or not 1 <= args.duration <= 60:
            raise ValueError("发送时长必须在 1–60 秒之间")
        if not math.isfinite(args.interval_ms) or not 1 <= args.interval_ms <= 200:
            raise ValueError("发送间隔必须在 1–200 毫秒之间")
        if not 1 <= args.port <= 65535:
            raise ValueError("UDP 端口必须在 1–65535 之间")
        ssid = args.ssid if args.ssid is not None else input("目标 2.4 GHz Wi-Fi 名称：")
        if args.open:
            password = ""
        else:
            if not sys.stdin.isatty():
                raise ValueError("请在终端交互输入密码；开放网络可用 --open")
            password = getpass.getpass("Wi-Fi 密码（输入不显示）：")
            if not password:
                raise ValueError("开放网络请明确使用 --open")
        record = encode(ssid, password, address)
        packets = data_packets(record)
        # No record/IP-destination dump: multicast destinations contain the key.
        print(f"编码完成：SSID {len(ssid.encode('utf-8'))} 字节，"
              f"V2 记录 {len(record)} 字节，{len(packets)} 对数据。")
        if args.dry_run:
            print("离线检查完成，未发送网络数据；尚未验证设备接收。")
            return 0
        print("普通 UDP 发包尚未验证成功；有条件建议使用官方 App，或按教程使用 Linux mac80211 帧注入。")
        print(f"从 Wi-Fi 网卡 {address} 发送，持续 {args.duration:g} 秒。")
        print("请让 A1 进入配网模式；发送期间保持电脑连接目标 2.4 GHz Wi-Fi。")
        count, cycles = send(
            packets, address, args.duration, args.interval_ms / 1000, args.port,
            progress=lambda n, c, elapsed: print(
                f"已发送 {n} 包，完整轮次 {c}，用时 {elapsed:.0f} 秒。", flush=True))
        print(f"发送结束：{count} 包，完整轮次 {cycles}。")
        print("这只是发送完成；请在路由器客户端列表确认 A1 是否获得 IP。")
        return 0
    except KeyboardInterrupt:
        print("\n发送已中止；设备是否完成配网需另行确认。", file=sys.stderr)
        return 130
    except EOFError:
        print("错误：未读取到交互输入。", file=sys.stderr)
        return 2
    except (ValueError, OSError) as exc:
        print(f"错误：{exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
