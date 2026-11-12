#!/usr/bin/env python3
"""Exercise the overlaid native startup function on normal and safe boots."""
import argparse
import json
from pathlib import Path
import subprocess

STUBS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
static char log_sink[256];
#define ADDLOGF_INFO(...) ((void)snprintf(log_sink,sizeof(log_sink),__VA_ARGS__))
#define ADDLOGF_DEBUG(...) ADDLOGF_INFO(__VA_ARGS__)
#define PH_BOOT_STAGE(stage) ((void)0)
static int bSafeMode, g_bootFailures, g_openAP, g_connectToWiFi;
static unsigned order, product, ticks, http, unsafe, connects;
static int product_safe_mode;
static char ssid[33];
static const char *CFG_GetWiFiSSIDX(void) {return ssid;}
static const char *CFG_GetWiFiPassX(void) {return "";}
static void HAL_Configure_WDT(void) {}
static bool Main_HasFastConnect(void) {return false;}
static void Main_ConnectToWiFiNow(void) {++connects;}
static void PH_BL_Init(int safe_mode) {
    assert(order==0);order=1;++product;product_safe_mode=safe_mode;
}
static void QuickTick_StartThread(void) {assert(order==1);order=2;++ticks;}
static void PH_BL_StartHTTP(void) {assert(order==2);order=3;++http;}
static void Main_Init_AfterDelay_Unsafe(bool late) {
    assert(late && !bSafeMode && order==3);++unsafe;
}
'''
CASES = r'''
int main(void) {
    for (unsigned safe=0;safe<2;++safe)for(unsigned configured=0;configured<2;++configured) {
        order=product=ticks=http=unsafe=connects=0;
        bSafeMode=safe;g_bootFailures=safe?8:0;g_openAP=g_connectToWiFi=0;
        strcpy(ssid,configured?"test-network":"");
        Main_Init_After_Delay();
        assert(order==3 && product==1 && ticks==1 && http==1);
        assert(product_safe_mode==(int)safe && unsafe==!safe && connects==0);
        assert((g_openAP==5)==(safe || !configured));
        assert((g_connectToWiFi==5)==(!safe && configured));
    }
    puts("PASS: actual native startup initializes the product and tick before HTTP, including safe mode");
}
'''

HEAP_STUBS = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "memory_policy.h"
#define PLATFORM_BEKEN 1
#define PLATFORM_W800 0
#define ADDLOGF_ERROR(...) ((void)0)
#define PH_BOOT_RESULT(stage, result) ((void)0)
#define PH_BOOT_STAGE(stage) ((void)0)
static unsigned heap_bytes, reboots, g_secondsSpentInLowMemoryWarning;
static unsigned xPortGetFreeHeapSize(void) {return heap_bytes;}
static void HAL_RebootModule(void) {++reboots;}
'''
HEAP_CASES = r'''
int main(void) {
    /* This is the measured post-service heap, below the native 25 kB floor. */
    heap_bytes=14000;
    for (unsigned i=0;i<180;++i) heap_tick();
    assert(reboots==0 && g_secondsSpentInLowMemoryWarning==0);
    heap_bytes=8191;
    for (unsigned i=0;i<5;++i) heap_tick();
    assert(reboots==0 && g_secondsSpentInLowMemoryWarning==5);
    heap_bytes=8192;heap_tick();
    assert(reboots==0 && g_secondsSpentInLowMemoryWarning==0);
    heap_bytes=8191;
    for (unsigned i=0;i<5;++i) heap_tick();
    assert(reboots==0);
    heap_tick();assert(reboots==1);
    puts("PASS: measured service heap stays online; sustained low heap still reboots after six checks");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app-source', type=Path, required=True)
    parser.add_argument('--run-dir', type=Path, required=True)
    args = parser.parse_args()
    text = (args.app_source/'src/user_main.c').read_text()
    start = text.index('void Main_Init_After_Delay()')
    end = text.index('\n// to be overriden', start)
    run = args.run_dir.resolve()
    run.mkdir(parents=True, exist_ok=True)
    source = run/'native-startup.c'
    source.write_text(STUBS+text[start:end]+CASES)
    executable = run/'native-startup'
    subprocess.run(['clang', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-function', '-fsanitize=address,undefined',
                    str(source), '-o', str(executable)], check=True)
    result = subprocess.run([str(executable)], text=True, capture_output=True, timeout=5)
    (run/'test.log').write_text(result.stdout+result.stderr)
    (run/'result.json').write_text(json.dumps({
        'status': 'PASS' if result.returncode==0 else 'FAIL',
        'native_after_delay_verbatim': True, 'asan_ubsan': True,
        'cases': ['normal configured', 'normal unconfigured',
                  'safe configured', 'safe unconfigured'],
    }, indent=2)+'\n')
    result.check_returncode()
    print(result.stdout, end='')
    # Compile the real overlaid native memory guard, not a copy of its logic.
    start = text.index('\t// On Beken, do reboot if we ran into heap size problem')
    end = text.index('\tif (bSafeMode == 0)', start)
    source = run/'native-heap-guard.c'
    source.write_text(HEAP_STUBS+'\nstatic void heap_tick(void) {\n'+text[start:end]+'\n}\n'+HEAP_CASES)
    executable = run/'native-heap-guard'
    subprocess.run(['clang', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
                    '-I', str(Path(__file__).resolve().parents[1]/'src'),
                    '-fsanitize=address,undefined', str(source), '-o', str(executable)], check=True)
    result = subprocess.run([str(executable)], text=True, capture_output=True, timeout=5)
    (run/'heap-test.log').write_text(result.stdout+result.stderr)
    (run/'heap-result.json').write_text(json.dumps({
        'status': 'PASS' if result.returncode==0 else 'FAIL',
        'native_heap_guard_verbatim': True, 'asan_ubsan': True,
        'cases': ['measured service heap for 180 seconds', 'five low-heap checks',
                  'threshold boundary and recovery', 'six sustained low-heap checks'],
    }, indent=2)+'\n')
    result.check_returncode()
    print(result.stdout, end='')


if __name__ == '__main__':
    main()
