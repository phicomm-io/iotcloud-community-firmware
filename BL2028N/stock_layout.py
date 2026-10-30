"""Default layout for the S30-V1B stock bootloader.

Only application OTA packages are emitted. No stock bootloader or device data
is redistributed. This path initializes fresh community settings; it does not
migrate settings from the earlier OpenBeken layout.
"""
import difflib
import struct

LAYOUT = {
    'name': 's30-v1b-stock-v1',
    'app_start': 0x11000, 'app_end': 0x132000,
    'ota_start': 0x132000, 'ota_size': 0xAE000,
    'factory_start': 0x1E0000, 'cfg_start': 0x1E2000,
    'lfs_start': 0x1F0000, 'lfs_end': 0x1F8000,
    'bootloader_included': False,
    'legacy_community_config_migration': False,
}


def application_image(raw, rbl_header):
    """Reproduce stock OTA installation for a bootloader-free UART image.

    Stock OTA stores unencrypted 32-byte application blocks with CRC16, then
    the original 96-byte RBL header at the end of the application partition.
    No bootloader, RF, factory identity or settings are included.
    """
    if len(rbl_header) != 96 or rbl_header[:4] != b'RBL\0':
        raise ValueError('A complete RBL header is required')
    if struct.unpack_from('<I', rbl_header, 84)[0] != len(raw):
        raise ValueError('RBL application size mismatch')

    def frame(data):
        out = bytearray()
        data = data + b'\xff' * (-len(data) % 32)
        for offset in range(0, len(data), 32):
            block = data[offset:offset + 32]
            crc = 0xffff
            for byte in block:
                crc ^= byte << 8
                for _ in range(8):
                    crc = ((crc << 1) ^ (0x8005 if crc & 0x8000 else 0)) & 0xffff
            out.extend(block + crc.to_bytes(2, 'big'))
        return bytes(out)

    capacity = LAYOUT['app_end'] - LAYOUT['app_start']
    payload, footer = frame(raw), frame(rbl_header)
    if len(payload) + len(footer) > capacity:
        raise ValueError('Application exceeds the stock application partition')
    return payload + b'\xff' * (capacity - len(payload) - len(footer)) + footer


def apply(app, sdk, run):
    changes = []

    def replace(root, relative, old, new):
        p = root / relative
        before = p.read_text()
        if before.count(old) != 1:
            raise ValueError('Stock layout patch no longer matches: ' + relative)
        after = before.replace(old, new)
        p.write_text(after)
        changes.extend(difflib.unified_diff(before.splitlines(True), after.splitlines(True),
                                            fromfile='a/' + relative, tofile='b/' + relative))

    replace(app, 'src/hal/hal_ota.h',
            '#elif PLATFORM_BK7231N\n#define START_ADR_OF_BK_PARTITION_OTA 0x12A000',
            '#elif PLATFORM_BK7231N\n#define START_ADR_OF_BK_PARTITION_OTA 0x132000')
    replace(app, 'src/littlefs/our_lfs.h',
            '#define LFS_BLOCKS_START 0x12B000\n#define LFS_BLOCKS_START_MIN 0x12B000\n'
            '// end of OTA flash\n#define LFS_BLOCKS_END 0x1D0000',
            '#define LFS_BLOCKS_START 0x1F0000\n#define LFS_BLOCKS_START_MIN 0x1F0000\n'
            '// Dedicated 32 KiB filesystem, outside the stock download partition.\n'
            '#define LFS_BLOCKS_END 0x1F8000')
    replace(app, 'src/hal/bk7231/hal_ota_bk7231.c',
            '      unsigned int lfs_size = CFG_GetLFS_Size();\n'
            '      unsigned int room = LFS_BLOCKS_END - START_ADR_OF_BK_PARTITION_OTA;\n'
            '      unsigned int limit = lfs_size < room ? (room - lfs_size) & ~4095u : 0;',
            '      /* Stock download ends before RF; LFS is in a separate region. */\n'
            '      unsigned int limit = 0xAE000u;')
    relative = 'platforms/bk7231n/bk7231n_os/beken378/func/user_driver/BkDriverFlash.c'
    replace(sdk, relative, '.partition_start_addr      = 0x12A000,',
            '.partition_start_addr      = 0x132000,')
    replace(sdk, relative, '.partition_length          = 0xA6000, //664KB',
            '.partition_length          = 0xAE000, //696KB, stock bootloader download')
    replace(sdk, relative, '.partition_start_addr      = 0x1D1000,// for net related info',
            '.partition_start_addr      = 0x1E2000,// community CFG; preserve stock NET info')
    (run / 'stock-layout.patch').write_text(''.join(changes))
