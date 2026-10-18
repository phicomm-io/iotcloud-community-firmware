#!/usr/bin/env python3
"""Validate/split Phicomm A1/M1 full images and run the zA1 OpenOCD tool.

Requires Python 3.9+, no third-party Python packages. See the A1/M1 guide.
The original vendor programmer is required; this script does not supply it.
"""
import argparse
import binascii
import hashlib
import re
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

PACKAGE_HASHES = {
    'sflash_write.tcl': '6081ffcc99fcf22b2c9efa94d03d744058d8a42088ed5ff4339950f586027c69',
    'flash_prog.elf': '9d051ac1d3e1dd55387930acb235fb90dea3d67a3e8696b8b311f463ee8b21eb',
    'openocd/scripts/interface/jlink_swd.cfg': '5b1dbdb5312dab1b0219cdb922475cd3d61e05c947cb12d9f721a574de5110cc',
    'openocd/scripts/target/MX1290/MX1290.cfg': '0928594ccd781b936312250c45091709b3623c826ef6aeecc88ded169b97de0f',
    'openocd/scripts/target/MX1290/core.cfg': '4ffafdb7ffb01b56007fad955c834345f201c19432868c7587a357b659fd2227',
    'openocd/scripts/mem_helper.tcl': '1f50b268c161ddd65cf0185ede97833705fc9492800a75e532c34b0733f655f7',
}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def split_image(image, model, checksums):
    require(image.name == f'phicomm_{model}.all.bin', 'Use the release .all.bin for the selected model')
    entries = []
    for line in checksums.read_text(encoding='utf-8').splitlines():
        match = re.fullmatch(r'([0-9a-fA-F]{64}) [ *](.+)', line)
        if match and match[2] in (image.name, './' + image.name):
            entries.append(match[1].lower())
    require(len(entries) == 1, 'SHA256SUMS must contain exactly one entry for this image')
    data = image.read_bytes()
    require(sha256(data) == entries[0], 'Release SHA256 mismatch')
    require(0xD0000 < len(data) <= 0x110000, 'Unsupported full-image length')
    require(data[0x12000:0x13000] == b'\xff' * 0x1000, 'Unexpected boot padding')
    require(data[0xC8000:0xD0000] == b'\xff' * 0x8000, 'Image contains parameter data; unsupported')
    length, reserved, crc, magic, stack, abi = struct.unpack_from('<IHHIII', data, 0x88000)
    require(120 <= length <= 0x40000 - 24, 'Unsupported application length')
    end = 0x88000 + 8 + length
    require(magic == 0xC89346 and abi == 3, 'Application magic or ABI mismatch')
    require(reserved == crc == binascii.crc_hqx(data[0x88008:end], 0), 'Application CRC mismatch')
    require(hashlib.md5(data[0x13000:end]).digest() == data[end:end+16], 'Embedded raw OTA MD5 mismatch')
    product = f'PHICOMM_{model.upper()}_EMW3080'.encode() + b'\0'
    require(product in data[0x88008:end], 'Application product mismatch')
    return {'boot.bin': data[:0x12000], 'ota.bin': data[0x13000:0xC8000], 'ate.bin': data[0xD0000:]}


def write_parts(parts, output):
    output.mkdir()  # Refuse to overwrite an existing firmware directory.
    for name, data in parts.items():
        (output / name).write_bytes(data)
        print(f'{name}: {len(data)} bytes, SHA256 {sha256(data)}', flush=True)
    (output / 'SHA256SUMS').write_text(''.join(f'{sha256(data)}  {name}\n' for name, data in parts.items()), encoding='utf-8')


def openocd_command(args, root):
    for name, expected in PACKAGE_HASHES.items():
        require(sha256((root / name).read_bytes()) == expected, f'Tool package mismatch: {name}')
    executable = args.openocd or ('openocd/win32/openocd_mico.exe' if args.engine == 'legacy' else 'openocd')
    candidate = Path(executable)
    if not candidate.is_absolute() and (root / candidate).is_file():
        executable = str((root / candidate).resolve())
    else:
        executable = shutil.which(executable) or str(candidate.resolve())
    require(Path(executable).is_file(), 'OpenOCD executable not found; set --openocd')
    command = [executable, '-s', '.', '-c', 'gdb_port 23333', '-c', 'telnet_port 24444', '-c', 'tcl_port 26666']
    if args.engine == 'legacy':
        command += ['-f', 'openocd/scripts/interface/jlink_swd.cfg', '-f', 'openocd/scripts/target/MX1290/MX1290.cfg']
    else:
        command += ['-f', str(Path(__file__).with_name('mx1290-modern.cfg').resolve())]
    return command


def run_openocd(command, root):
    print('Command: ' + (subprocess.list2cmdline(command) if sys.platform == 'win32' else shlex.join(command)), flush=True)
    subprocess.run(command, cwd=root, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='action', required=True)
    for action in ('split', 'check', 'flash'):
        p = sub.add_parser(action)
        if action != 'check':
            p.add_argument('image', type=Path)
            p.add_argument('--model', choices=('a1', 'm1'), required=True)
            p.add_argument('--checksums', type=Path, required=True)
        if action == 'split':
            p.add_argument('--output', type=Path, default=Path('firmware'))
        else:
            p.add_argument('--tool-dir', type=Path, required=True)
            p.add_argument('--engine', choices=('legacy', 'modern'), required=True)
            p.add_argument('--openocd', help='OpenOCD executable; defaults to bundled Windows binary or PATH')
    args = parser.parse_args()
    if args.action == 'split':
        write_parts(split_image(args.image, args.model, args.checksums), args.output)
        return
    parts = split_image(args.image, args.model, args.checksums) if args.action == 'flash' else None
    root = args.tool_dir.resolve()
    command = openocd_command(args, root)
    if args.action == 'check':
        run_openocd(command + ['-c', 'init', '-c', 'shutdown'], root)
        return
    # Relative generated ASCII paths avoid Tcl quoting problems with user paths.
    with tempfile.TemporaryDirectory(prefix='phicomm-', dir=root) as temp:
        output = Path(temp) / 'firmware'
        write_parts(parts, output)
        command += ['-f', 'sflash_write.tcl']
        for partition, name in enumerate(('boot.bin', 'ota.bin', 'ate.bin'), 1):
            relative = (output / name).relative_to(root).as_posix()
            command += ['-c', f'sflash_write_file {relative} {partition} 0x0 MK3080B 0']
        command += ['-c', 'reset run', '-c', 'shutdown']
        run_openocd(command, root)
    print('Write and programmer post-write verification completed. Check normal boot and device functions.')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f'ERROR: {error}', file=sys.stderr)
        sys.exit(1)
