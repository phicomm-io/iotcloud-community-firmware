"""Execute the buffered sender with fake UART/time; compare every wire byte."""
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def crc_reference(data):
    # Polynomial long division, without appending the usual sixteen zero bits.
    remainder = 0
    for byte in data:
        remainder = (remainder << 8) | byte
        for bit in range(23, 15, -1):
            if remainder & (1 << bit):
                remainder ^= 0x11021 << (bit - 16)
    return remainder


class BulkSenderTest(unittest.TestCase):
    def test_uart_packets_failures_and_deadlines(self):
        source = (ROOT / 'M1/http.c').read_text()
        sender = source[source.index('static bool mcu_bulk('):source.index('static ph_http_action prepare(')]
        cases = []
        for size in (164, 256, 512, 26052, 32768):
            data = struct.pack('<II', 0x20008000, 0x4041) + bytes((i * 37 ^ (i >> 8)) & 255 for i in range(size - 8))
            packets = [bytes((0x23, 0x12, size // 256, size % 256)) + bytes(7) + b'\x21']
            for index in range(size // 256 + 1):
                chunk = data[index * 256:(index + 1) * 256]
                packets.append(b'\x23\x13' + struct.pack('>H', index + 1) + chunk + struct.pack('>H', crc_reference(chunk)))
            packets.append(b'\x23\x16' + bytes(9) + b'\x21')
            cases.append((data, packets))
        c = '#include <stdint.h>\n#include <stddef.h>\n#include <stdbool.h>\n#include <string.h>\n#include <assert.h>\n'
        for ci, (data, packets) in enumerate(cases):
            c += f'static uint8_t image{ci}[]={{' + ','.join(map(str, data)) + '};\n'
            for pi, packet in enumerate(packets):
                c += f'static const uint8_t c{ci}p{pi}[]={{' + ','.join(map(str, packet)) + '};\n'
            c += f'static const uint8_t *packets{ci}[]={{' + ','.join(f'c{ci}p{pi}' for pi in range(len(packets))) + '};\n'
            c += f'static const unsigned lengths{ci}[]={{' + ','.join(str(len(p)) for p in packets) + '};\n'
        c += r'''
static const uint8_t **packets;static const unsigned *lengths;
static unsigned pos,commits,cancels,now,delay,fail_at,bad_ack;
static uint32_t mico_rtos_get_time(void){return now;}
static void mico_thread_msleep(unsigned n){now+=n;}
static bool m1_mcu_exchange(const uint8_t *p,size_t n,uint8_t out[20]){
    now+=delay;memset(out,0,20);out[0]=0x23;out[19]=0x21;
    if(p[1]==0x16 && p[2]==1){cancels++;out[1]=0x1a;return true;}
    assert(n==lengths[pos]);assert(!memcmp(p,packets[pos],n));
    if(pos==fail_at)return false;
    if(p[1]==0x16){commits++;out[1]=0x16;}
    else{out[1]=0x14;if(p[1]==0x13){out[2]=p[2];out[3]=p[3];out[4]=1;}}
    if(pos==bad_ack)out[3]^=1;
    pos++;return true;
}
static void reset(void){pos=commits=cancels=now=0;delay=40;fail_at=bad_ack=~0u;}
'''
        c += sender + '\nint main(void){\n'
        for ci, (_, packets) in enumerate(cases):
            c += f'reset();packets=packets{ci};lengths=lengths{ci};assert(mcu_bulk(image{ci},sizeof(image{ci})));assert(commits==1 && !cancels && pos=={len(packets)});\n'
        c += '''
packets=packets3;lengths=lengths3;
reset();fail_at=0;assert(!mcu_bulk(image3,sizeof(image3)));assert(!commits && cancels==1);
reset();fail_at=8;assert(!mcu_bulk(image3,sizeof(image3)));assert(!commits && cancels==1);
reset();bad_ack=8;assert(!mcu_bulk(image3,sizeof(image3)));assert(!commits && cancels==1);
reset();delay=200;assert(!mcu_bulk(image3,sizeof(image3)));assert(!commits && cancels==1);
reset();assert(!mcu_bulk(image3,163));assert(!mcu_bulk(image3,32769));assert(!pos && !cancels);
reset();image3[0]=image3[1]=image3[2]=image3[3]=0;assert(!mcu_bulk(image3,sizeof(image3)));assert(!pos && !commits && !cancels);
return 0;}
'''
        with tempfile.TemporaryDirectory() as directory:
            src = Path(directory) / 'bulk.c'
            exe = src.with_suffix('')
            src.write_text(c)
            subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-no-pie', str(src), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()
