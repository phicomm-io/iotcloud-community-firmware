"""Exercise the production UART clock recovery without DNS or FIFO progress."""
from pathlib import Path
import subprocess
import tempfile

source = Path('M1/uart.c').read_text()
routine = source[source.index('/* Only the UART worker owns'):source.index('/* Outside the FIFO: retry persisted')]
harness = r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <stdio.h>
#include <string.h>
#define custom_log(...) ((void)0)
typedef uint64_t mico_utc_time_ms_t;
typedef struct { int timezone_minutes; } m1_config_t;
#define kNoErr 0
static int m1_lock;
static struct { bool ntp_synced, mcu_updating, ota_active, reboot_pending; } m1_state;
static uint64_t utc = 1704067200000ULL;
static int timezone_minutes=480, sends, hour, second, queries;
static uint8_t reply[20];
static bool send_ok=true, read_ok=true;
static void mico_rtos_lock_mutex(int *m){(void)m;}
static void mico_rtos_unlock_mutex(int *m){(void)m;}
static int mico_time_get_utc_time_ms(uint64_t *out){*out=utc; return read_ok?0:-1;}
static void m1_config_get(m1_config_t *cfg){cfg->timezone_minutes=timezone_minutes;}
static void m1_clock_frame(uint8_t *f,int y,int mo,int d,int h,int mi,int s){
 hour=h;second=s;memset(f,0,12);f[0]=0x23;f[1]=3;f[11]=0x21;
 f[2]=y>>8;f[3]=y;f[4]=mo;f[5]=d;f[6]=h;f[7]=mi;f[8]=s;
 memcpy(reply,f,12);reply[1]=0x0d;reply[19]=0x21;
}
static bool transmit(const uint8_t *f){
 assert(f[0]==0x23 && f[11]==0x21);
 if(f[1]==3)sends++;else {assert(f[1]==0x0d);queries++;}
 return send_ok;
}
'''
checks = r'''
static void reset(void){
 memset(&screen_clock,0,sizeof(screen_clock));
 memset(&m1_state,0,sizeof(m1_state));m1_state.ntp_synced=true;
 sends=queries=0;send_ok=read_ok=true;utc=1704067200000ULL;timezone_minutes=480;
}
int main(void){
 reset();screen_clock_sync(0,false);assert(sends==0); /* No periodic sends. */
 screen_clock_sync(0,true);assert(sends==1 && hour==8);
 screen_clock_sync(249,false);assert(queries==0);
 screen_clock_sync(250,false);assert(queries==1);
 assert(screen_clock_rx(reply,300));assert(!screen_clock.active);
 screen_clock_sync(3600000,false);assert(sends==1); /* No 60s refresh. */
 reset();screen_clock_sync(0,true);screen_clock_sync(250,false);
 reply[1]=0x0c;assert(screen_clock_rx(reply,300));assert(screen_clock.active);
 reply[1]=0x0d;reply[3]=0xd0; /* RTC still in 2000, retry. */
 screen_clock_rx(reply,300);assert(screen_clock.active && !screen_clock.waiting);
 utc+=5000;timezone_minutes=0;screen_clock_sync(1299,false);assert(sends==1);
 screen_clock_sync(1300,false);assert(sends==2 && hour==0 && second==5);
 screen_clock_sync(1550,false);screen_clock_rx(reply,1600);assert(!screen_clock.active);
 reset();screen_clock_sync(0,true);
 for(unsigned i=0;i<3;i++){
  unsigned start=i*3250u;
  screen_clock_sync(start+250,false);screen_clock_sync(start+2249,false);
  assert(screen_clock.active);
  screen_clock_sync(start+2250,false); /* timeout */
  if(i<2)screen_clock_sync(start+3250,false);
 }
 assert(sends==3 && queries==3 && !screen_clock.active);
 screen_clock_sync(999999,false);assert(sends==3);
 reset();send_ok=false;screen_clock_sync(0,true);
 screen_clock_sync(1000,false);screen_clock_sync(2000,false);
 assert(sends==3 && queries==0 && !screen_clock.active);
 reset();screen_clock_sync(0,true);send_ok=false;screen_clock_sync(250,false);
 assert(!screen_clock.waiting && !screen_clock.query_pending);
 send_ok=true;screen_clock_sync(1250,false);assert(sends==2);
 reset();read_ok=false;screen_clock_sync(0,true);
 screen_clock_sync(1000,false);screen_clock_sync(2000,false);
 assert(sends==0 && !screen_clock.active);
 reset();m1_state.ntp_synced=false;screen_clock_sync(0,true);assert(sends==0);
 m1_state.ntp_synced=true;screen_clock_sync(1,false);screen_clock_sync(1001,false);assert(sends==1);
 reset();screen_clock_sync(0,true);m1_state.mcu_updating=true;
 screen_clock_sync(250,false);assert(queries==0);
 screen_clock_rx(reply,300);assert(screen_clock.active);
 m1_state.mcu_updating=false;screen_clock_sync(5000,false);
 screen_clock_sync(6000,false);assert(sends==2 && queries==0);
 m1_state.ota_active=true;screen_clock_sync(6250,false);assert(queries==0);
 m1_state.ota_active=false;m1_state.reboot_pending=true;
 screen_clock_sync(7000,true);assert(sends==2);
 reset();utc=1735689599000ULL;timezone_minutes=0; /* 2024-12-31 23:59:59 */
 screen_clock_sync(0,true);screen_clock_sync(250,false);utc+=2000;
 screen_clock_rx(reply,300);assert(!screen_clock.active); /* day/year boundary tolerance */
 reset();screen_clock_sync(0,true);screen_clock_sync(250,false);
 reply[4]=13;screen_clock_rx(reply,300);assert(screen_clock.active && !screen_clock.waiting);
 reset();screen_clock_sync(UINT32_MAX-100u,true);
 screen_clock_sync(148u,false);assert(queries==0);
 screen_clock_sync(149u,false);assert(queries==1);
 screen_clock_rx(reply,150);assert(!screen_clock.active);
 reset();screen_clock_sync(0,true);screen_clock_sync(250,false);
 screen_clock_rx(reply,2250);assert(screen_clock.active && !screen_clock.waiting); /* expired */
 puts("M1 screen clock: readback, mismatch, timeout, bounded retries, pause, rollover and no periodic writes passed");
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / 'clock.c').write_text(harness + routine + checks)
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', str(path / 'clock.c'), '-o', str(path / 'clock')], check=True)
    subprocess.run([str(path / 'clock')], check=True)
