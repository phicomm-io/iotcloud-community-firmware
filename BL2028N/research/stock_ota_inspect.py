#!/usr/bin/env python3
"""Verify selected static facts in one private S30-V1B stock application.

Input is an already decoded application, not a flashable image. No network or
device I/O is performed. Arbitrary strings and key material are never emitted.
"""

import argparse
import hashlib
import json
from pathlib import Path
import struct

from capstone import CS_ARCH_ARM, CS_MODE_THUMB, Cs
from capstone.arm import ARM_OP_IMM


BASE = 0x10000
APP_BYTES = 1079392
APP_SHA256 = "6cd7246861ea44f277f25bc61b2f236cec7c5f4db3c487cdb177c8bbc9c9e723"
CALLS = (
    ("andlink_url_to_ota", 0x15664, 0x19C14),
    ("orvibo_url_to_ota", 0x18E0E, 0x19C14),
    ("ota_to_http_download", 0x19C30, 0x5E838),
    ("download_http_request", 0x5E880, 0x28B04),
    ("download_success_reboot", 0x5E88E, 0x5D824),
    ("serial_cli_to_http_download", 0x3EDE2, 0x5E838),
    ("cloud_tcp_connect", 0x1C792, 0x2C828),
)
PARTITIONS = (
    ("Bootloader", 0, 0xF000),
    ("Application", 0x11000, 0x143000),
    ("ota", 0x132000, 0xAE000),
    ("RF Firmware", 0x1E0000, 0x1000),
    ("NET info", 0x1E1000, 0x1000),
)


def inspect(data):
    digest = hashlib.sha256(data).hexdigest()
    if len(data) != APP_BYTES or digest != APP_SHA256:
        raise ValueError("Input does not match the documented stock build")

    def read(address, size):
        offset = address - BASE
        if offset < 0 or offset + size > len(data):
            raise ValueError("Reference outside decoded application")
        return data[offset:offset + size]

    def cstring(address):
        raw = read(address, 64)
        if b"\0" not in raw:
            raise ValueError("Unterminated table name")
        return raw.split(b"\0", 1)[0].decode("ascii")

    md = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
    md.detail = True
    calls = []
    for label, site, target in CALLS:
        insn = next(md.disasm(read(site, 4), site, count=1), None)
        if (insn is None or insn.mnemonic != "bl" or
                insn.operands[0].type != ARM_OP_IMM or
                insn.operands[0].imm != target):
            raise ValueError(f"Expected direct call not found: {label}")
        calls.append({"label": label, "site": hex(site), "target": hex(target)})

    partitions = []
    for index, (name, start, size) in enumerate(PARTITIONS):
        owner, name_ptr, actual_start, actual_size, options = struct.unpack(
            "<5I", read(0xE3B84 + index * 20, 20))
        if (cstring(name_ptr), actual_start, actual_size) != (name, start, size):
            raise ValueError(f"Partition descriptor mismatch: {index}")
        partitions.append({"index": index, "name": name, "owner": owner,
                           "start": hex(start), "length": hex(size),
                           "options": hex(options)})

    callback = struct.unpack("<I", read(0x184C8, 4))[0]
    if callback != 0x188B5:
        raise ValueError("Cloud receive callback literal mismatch")
    if cstring(0xF9FB8) != "homemate.orvibo.com":
        raise ValueError("Default cloud host mismatch")
    port = struct.unpack("<I", read(0xF9FE8, 4))[0]
    if port != 10001:
        raise ValueError("Default cloud port mismatch")

    return {
        "status": "STATIC_FACTS_VERIFIED",
        "input_bytes": len(data), "input_sha256": digest,
        "logical_base": hex(BASE),
        "direct_calls": calls,
        "cloud_receive_callback_thumb_pointer": hex(callback),
        "default_cloud_host": "homemate.orvibo.com",
        "default_cloud_port": port,
        "partition_table_address": "0xe3b84", "partitions": partitions,
        "application_descriptor_overlaps_ota": True,
        "packet_crypto_verified": False,
        "bootloader_ota_acceptance_verified": False,
        "physical_stock_wireless_migration_verified": False,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    try:
        result = inspect(args.app.read_bytes())
    except (OSError, ValueError) as error:
        parser.exit(1, f"Static verification failed: {error}\n")
    encoded = json.dumps(result, indent=2, ensure_ascii=True) + "\n"
    if args.output:
        args.output.write_text(encoded)
    else:
        print(encoded, end="")


if __name__ == "__main__":
    main()
