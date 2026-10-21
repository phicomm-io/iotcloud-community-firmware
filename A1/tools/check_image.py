#!/usr/bin/env python3
"""Read-only validation of native A1 OTA images, mirroring the device validator."""
import hashlib
import struct
import sys
from pathlib import Path

OFFSET = 0x75000
BASE = 0x08088000
PRODUCT = b"PHICOMM_A1_EMW3080\0"
# MK3080B OTA_TEMP is 0xb5000 bytes, including the native trailing MD5.
STAGING_LIMIT = 0xb5000
KERNEL_SHA256 = "f22ad344286bb97755ab7ba2fb18e8768140c5383875098dfb0449d2184abc74"


def crc16(data):
    value = 0
    for byte in data:
        value ^= byte << 8
        for _ in range(8):
            value = ((value << 1) ^ (0x1021 if value & 0x8000 else 0)) & 0xffff
    return value


def check(path):
    image = Path(path).read_bytes()
    if not OFFSET + 144 <= len(image) <= min(OFFSET + 0x40000 + 16, STAGING_LIMIT):
        raise ValueError("Invalid OTA length")
    if hashlib.sha256(image[:OFFSET]).hexdigest() != KERNEL_SHA256:
        raise ValueError("Unexpected kernel/padding")
    length, a, b, magic, stack, abi = struct.unpack_from("<IHHIII", image, OFFSET)
    if length < 120 or len(image) != OFFSET + 8 + length + 16 or magic != 0xc89346 or abi != 3:
        raise ValueError("Invalid application header/ABI")
    if a != b or a != crc16(image[OFFSET + 8:-16]):
        raise ValueError("Application CRC mismatch")
    pointer = struct.unpack_from("<I", image, OFFSET + 28)[0]
    relative = pointer - BASE
    if not 8 <= relative <= length + 8 - len(PRODUCT) or image[OFFSET + relative:OFFSET + relative + len(PRODUCT)] != PRODUCT:
        raise ValueError("Wrong product identifier")
    if hashlib.md5(image[:-16]).digest() != image[-16:]:
        raise ValueError("OTA MD5 mismatch")
    return {"size": len(image), "application_payload": length, "abi": abi,
            "stack": stack, "sha256": hashlib.sha256(image).hexdigest()}


if __name__ == "__main__":
    try:
        print(check(sys.argv[1]))
    except (ValueError, IndexError, OSError) as error:
        sys.exit(str(error))
