#!/usr/bin/env python3
"""Gate DAIKIN OTA images against stock or explicitly selected wired layouts."""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
FLASH_RESERVE = 32 * 1024
STATIC_RAM_LIMIT = 96 * 1024
STOCK_OTA_SLOT_BYTES = 0x180000


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


def measure(build, partitions, size_tool, variant, layout='stock'):
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
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    build = args.build_dir or ROOT / 'DAIKIN/.pio/build' / ('bry88ab-hub' if args.variant == 'hub' else 'bry88ab')
    failure = None
    try:
        report = measure(build, args.partitions, args.size_tool, args.variant, args.layout)
    except BudgetError as error:
        report, failure = error.report, error
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, sort_keys=True))
    if failure:
        parser.exit(1, str(failure) + '\n')
