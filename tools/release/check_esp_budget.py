#!/usr/bin/env python3
"""Gate DAIKIN OTA images against stock or explicitly selected wired layouts."""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]
FLASH_RESERVE = 32 * 1024
STATIC_RAM_LIMIT = 96 * 1024
STOCK_OTA_SLOT_BYTES = 0x180000
HUB_LOOP_STACK_FRAME_LIMIT = 1024


class BudgetError(ValueError):
    def __init__(self, report, errors):
        self.report = report
        super().__init__('; '.join(errors))


def number(text):
    text = text.strip().lower()
    scale = 1024 if text.endswith('k') else 1024 * 1024 if text.endswith('m') else 1
    return int(text[:-1] if scale != 1 else text, 0) * scale


def ota_capacity(partitions):
    slots = []
    with Path(partitions).open() as source:
        for row in csv.reader(line for line in source if line.strip() and not line.lstrip().startswith('#')):
            if len(row) >= 5 and row[1].strip() == 'app' and row[2].strip().startswith('ota_'):
                slots.append(number(row[4]))
    if len(slots) < 2 or min(slots) <= FLASH_RESERVE:
        raise ValueError('Two usable OTA application partitions are required')
    return min(slots)


def static_ram(section_sizes):
    sections = {}
    for line in section_sizes.splitlines():
        fields = line.split()
        if len(fields) >= 2 and fields[0].startswith('.'):
            sections[fields[0]] = int(fields[1])
    if not {'.dram0.data', '.dram0.bss'} <= sections.keys():
        raise ValueError('Missing ESP32 RAM sections in ELF size output')
    return sum(sections.get(name, 0) for name in ('.dram0.data', '.dram0.bss', '.noinit'))


def loop_stack_frame(disassembly):
    """Read the resident loop frame, where LTO must not hoist HAP buffers."""
    symbol = re.search(r'^\s*[0-9a-fA-F]+ <_Z4loopv>:\s*$', disassembly, re.MULTILINE)
    if not symbol:
        raise ValueError('Missing loop symbol in ELF disassembly')
    body = disassembly[symbol.end():].strip().splitlines()
    # The Xtensa windowed ABI encodes the complete local frame in its first
    # instruction. Do not accidentally accept an entry from a later function.
    entry = re.fullmatch(r'\s*[0-9a-fA-F]+:\s+[0-9a-fA-F]+\s+entry\s+a1,\s*(0[xX][0-9a-fA-F]+|[0-9]+)\s*',
                         body[0] if body else '')
    if not entry:
        raise ValueError('Missing loop entry instruction in ELF disassembly')
    frame = int(entry.group(1), 16 if entry.group(1).lower().startswith('0x') else 10)
    if not frame or frame % 16:
        raise ValueError('Invalid aligned loop stack frame in ELF disassembly')
    return frame


def measure(build, partitions, size_tool, variant, layout='stock', objdump_tool=None):
    if layout not in ('stock', 'wired'):
        raise ValueError('Unknown OTA partition layout')
    build = Path(build)
    application = build / 'firmware.bin'
    elf = build / 'firmware.elf'
    image = application.read_bytes()
    elf_body = elf.read_bytes()
    if not image or not elf_body:
        raise ValueError('Application and ELF must contain real build outputs')
    configured_capacity = ota_capacity(partitions)
    # An application OTA does not replace the installed partition table. The
    # stock migration path therefore retains its smaller slots indefinitely.
    capacity = min(configured_capacity, STOCK_OTA_SLOT_BYTES) if layout == 'stock' else configured_capacity
    ram = static_ram(subprocess.check_output([str(size_tool), '-A', str(elf)], text=True))
    report = dict(schema=1, model='daikin', firmware_variant=variant,
                  application_bytes=len(image), application_sha256=hashlib.sha256(image).hexdigest(),
                  elf_sha256=hashlib.sha256(elf_body).hexdigest(), ota_layout=layout,
                  ota_slot_bytes=capacity, configured_ota_slot_bytes=configured_capacity,
                  stock_ota_slot_bytes=STOCK_OTA_SLOT_BYTES,
                  stock_ota_compatible=len(image) + FLASH_RESERVE <= min(configured_capacity, STOCK_OTA_SLOT_BYTES),
                  wired_ota_compatible=len(image) + FLASH_RESERVE <= configured_capacity,
                  flash_reserve_bytes=FLASH_RESERVE, static_ram_bytes=ram,
                  static_ram_limit_bytes=STATIC_RAM_LIMIT)
    errors = []
    if len(image) + FLASH_RESERVE > capacity:
        errors.append(f'{variant}: application {len(image)} + reserve {FLASH_RESERVE} exceeds {layout} OTA slot {capacity}')
    if ram > STATIC_RAM_LIMIT:
        errors.append(f'{variant}: static RAM {ram} exceeds budget {STATIC_RAM_LIMIT}')
    if variant == 'hub':
        report['loop_stack_frame_bytes'] = None
        report['loop_stack_frame_limit_bytes'] = HUB_LOOP_STACK_FRAME_LIMIT
        try:
            if objdump_tool is None:
                size_path = Path(size_tool)
                if not size_path.name.endswith('-size'):
                    raise ValueError('Specify --objdump-tool for a nonstandard size tool name')
                objdump_tool = size_path.with_name(size_path.name[:-5] + '-objdump')
            disassembly = subprocess.check_output([str(objdump_tool), '-d', '--disassemble=_Z4loopv', str(elf)], text=True)
            frame = loop_stack_frame(disassembly)
            report['loop_stack_frame_bytes'] = frame
            if frame > HUB_LOOP_STACK_FRAME_LIMIT:
                errors.append(f'hub: loop stack frame {frame} exceeds budget {HUB_LOOP_STACK_FRAME_LIMIT}')
        except (OSError, subprocess.CalledProcessError, ValueError) as error:
            errors.append(f'hub: cannot measure loop stack frame: {error}')
    report['passed'] = not errors
    if errors:
        report['errors'] = errors
        raise BudgetError(report, errors)
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--variant', choices=['full', 'hub'], required=True)
    parser.add_argument('--build-dir', type=Path)
    parser.add_argument('--partitions', type=Path, default=ROOT / 'DAIKIN/partitions.csv')
    parser.add_argument('--layout', choices=['stock', 'wired'], default='stock',
                        help='Stock migration keeps 0x180000 slots; wired uses the supplied partition CSV')
    core = Path(os.environ.get('PLATFORMIO_CORE_DIR', Path.home() / '.platformio'))
    parser.add_argument('--size-tool', type=Path, default=core / 'packages/toolchain-xtensa-esp32/bin/xtensa-esp32-elf-size')
    parser.add_argument('--objdump-tool', type=Path,
                        help='Default: the Xtensa objdump beside --size-tool; required for the Hub loop frame gate')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    build = args.build_dir or ROOT / 'DAIKIN/.pio/build' / ('bry88ab-hub' if args.variant == 'hub' else 'bry88ab')
    failure = None
    try:
        report = measure(build, args.partitions, args.size_tool, args.variant, args.layout, args.objdump_tool)
    except BudgetError as error:
        report, failure = error.report, error
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, sort_keys=True))
    if failure:
        parser.exit(1, str(failure) + '\n')
