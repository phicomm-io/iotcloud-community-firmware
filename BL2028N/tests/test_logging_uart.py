#!/usr/bin/env python3
"""Exercise the native buffered logger with independent UART backpressure."""
import argparse
import json
from pathlib import Path
import re
import subprocess

STUBS = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define PLATFORM_ARMINO 0
#define UART1_PORT 0
#define UART2_PORT 1
#define LOGTYPE_NONE 0
#define LOGTYPE_THREAD 2
#define LOGSIZE 4096
#define pdTRUE 1
typedef int BaseType_t;
static int initialised=1, direct_serial_log=LOGTYPE_THREAD;
static unsigned transmitted[2], pending[2], locked;
static struct {char log[LOGSIZE];int head,tailserial,mutex;} logMemory;
static int xSemaphoreTake(int mutex,unsigned ticks) {
    (void)mutex;assert(ticks==100 && !locked);locked=1;return pdTRUE;
}
static void xSemaphoreGive(int mutex) {(void)mutex;assert(locked);locked=0;}
static int uart_is_tx_fifo_full(unsigned port) {assert(port<2);return pending[port]>=16;}
static void write_byte(unsigned port,char value) {
    assert(locked && port==UART1_PORT && value=='L' && pending[port]<16);
    ++pending[port];++transmitted[port];
}
#define UART_WRITE_BYTE(port,value) write_byte(port,value)
'''
CASES = r'''
int main(void) {
    /* A full meter TX FIFO must not stall logging or receive log bytes. */
    pending[UART2_PORT]=16;
    memset(logMemory.log,'L',sizeof(logMemory.log));logMemory.head=600;
    unsigned calls=0;
    while(getSerial2()) {assert(++calls<100);pending[UART1_PORT]=0;}
    assert(calls>1 && transmitted[0]==600 && transmitted[1]==0 && !locked);
    /* Conversely, logger backpressure must be checked on UART1. */
    logMemory.head=601;pending[UART1_PORT]=16;pending[UART2_PORT]=0;
    assert(getSerial2() && logMemory.tailserial==600 && !locked);
    pending[UART1_PORT]=0;assert(!getSerial2());
    assert(transmitted[0]==601 && transmitted[1]==0);
    direct_serial_log=LOGTYPE_NONE;logMemory.head=602;
    assert(!getSerial2() && transmitted[0]==601 && transmitted[1]==0);
    puts("PASS: native logger drains UART1, respects its backpressure, and never transmits on meter UART2");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app-source', type=Path, required=True)
    parser.add_argument('--run-dir', type=Path, required=True)
    args = parser.parse_args()
    text = (args.app_source/'src/logging/logging.c').read_text()
    selectors = re.search(r'int UART_PORT = [^;]+;\nint UART_PORT_INDEX = [^;]+;', text)
    assert selectors
    start = text.index('static int getSerial2() {')
    body = text.index('{', start)
    depth = 1
    end = body+1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    run = args.run_dir.resolve()
    run.mkdir(parents=True, exist_ok=True)
    source = run/'native-logging-uart.c'
    source.write_text(STUBS+selectors.group(0)+'\n'+text[start:end]+CASES)
    executable = run/'native-logging-uart'
    subprocess.run(['clang', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', str(source), '-o', str(executable)], check=True)
    result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=5)
    (run/'test.log').write_text(result.stdout+result.stderr)
    report = {'status': 'PASS' if result.returncode == 0 else 'FAIL',
              'native_logger_verbatim': True, 'native_port_selectors_verbatim': True,
              'asan_ubsan': True, 'hardware_validation': False,
              'cases': ['meter UART full', 'logger UART full', 'continued drain', 'serial disabled']}
    (run/'result.json').write_text(json.dumps(report, indent=2)+'\n')
    result.check_returncode()
    print(result.stdout, end='')


if __name__ == '__main__':
    main()
