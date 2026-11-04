"""Exercise the production UART clock recovery without DNS or FIFO progress."""
from pathlib import Path
import subprocess
import tempfile

source = Path('M1/uart.c').read_text()
routine = source[source.index('static void screen_clock_sync'):source.index('/* Outside the FIFO: retry persisted')]
harness = r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <stdio.h>
typedef uint64_t mico_utc_time_ms_t;
typedef struct { int timezone_minutes; } m1_config_t;
#define kNoErr 0
static int m1_lock;
static struct { bool ntp_synced, mcu_updating, ota_active, reboot_pending; } m1_state;
static uint64_t utc = 1704067200000ULL;
static int timezone_minutes=480, sends, hour, second;
static bool send_ok=true, read_ok=true;
static void mico_rtos_lock_mutex(int *m){(void)m;}
static void mico_rtos_unlock_mutex(int *m){(void)m;}
static int mico_time_get_utc_time_ms(uint64_t *out){*out=utc; return read_ok?0:-1;}
static void m1_config_get(m1_config_t *cfg){cfg->timezone_minutes=timezone_minutes;}
static void m1_clock_frame(uint8_t *f,int y,int mo,int d,int h,int mi,int s){
 assert(y==2024 && mo==1 && d==1 && mi==0);hour=h;second=s;f[0]=3;
}
static bool transmit(const uint8_t *f){assert(f[0]==3);sends++;return send_ok;}
'''
checks = r'''
int main(void){
 screen_clock_sync(0,false); assert(sends==0);
 m1_state.ntp_synced=true;
 screen_clock_sync(1,false); assert(sends==1 && hour==8);
 screen_clock_sync(60000,false); assert(sends==1);
 utc+=5000; screen_clock_sync(60001,false); assert(sends==2 && second==5);
 timezone_minutes=0; screen_clock_sync(60002,true); assert(sends==3 && hour==0);
 send_ok=false; screen_clock_sync(60003,true); assert(sends==4);
 screen_clock_sync(61002,false); assert(sends==4);
 send_ok=true; screen_clock_sync(61003,false); assert(sends==5);
 m1_state.mcu_updating=true; screen_clock_sync(121003,false);assert(sends==5);
 m1_state.mcu_updating=false; m1_state.ota_active=true;
 screen_clock_sync(121004,true);assert(sends==5);
 m1_state.ota_active=false; m1_state.reboot_pending=true;
 screen_clock_sync(121005,true);assert(sends==5);
 m1_state.reboot_pending=false; read_ok=false;
 screen_clock_sync(121006,false); assert(sends==5);
 read_ok=true; screen_clock_sync(121007,false); assert(sends==6);
 screen_clock_sync(UINT32_MAX-500u,true); assert(sends==7);
 screen_clock_sync(59498u,false); assert(sends==7);
 screen_clock_sync(59499u,false); assert(sends==8);
 puts("M1 screen clock: startup, offline refresh, current time/timezone, send retry, OTA guards, wrap passed");
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / 'clock.c').write_text(harness + routine + checks)
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', str(path / 'clock.c'), '-o', str(path / 'clock')], check=True)
    subprocess.run([str(path / 'clock')], check=True)
