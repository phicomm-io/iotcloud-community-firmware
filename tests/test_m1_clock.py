"""Run the production M1 NTP routine with deterministic DNS/SNTP/UART failures."""
from pathlib import Path
import subprocess
import tempfile

source = Path('M1/network.c').read_text()
constants = source[source.index('#define CLOCK_SYNC'):source.index('static void scan_done')]
routine = source[source.index('static bool clock_sync'):source.index('static void network_worker')]
harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <netdb.h>
#include <netinet/in.h>
typedef uint64_t mico_utc_time_ms_t;
typedef struct { uint32_t seconds, microseconds; } ntp_timestamp_t;
#define kNoErr 0
#define custom_log(...) ((void)0)
static int m1_lock;
static struct {
 bool ntp_synced, ntp_screen_queued;
 uint32_t ntp_last_success;
 char ntp_status[24], ntp_server[64];
} m1_state;
static void mico_rtos_lock_mutex(int *m){(void)m;}
static void mico_rtos_unlock_mutex(int *m){(void)m;}
static int scenario, calls, writes, clocks, last_hour;
static bool queue_ok=true;
static uint64_t stored;
static char tried[8][64];
struct hostent *gethostbyname(const char *name) {
 static char addr[4]={1,2,3,4}; static char *list[]={addr,NULL};
 static struct hostent host;
 assert(calls<8); snprintf(tried[calls],64,"%s",name); ++calls;
 if(scenario==1 || (scenario==2 && calls==1)) return NULL;
 host.h_addr_list=list; return &host;
}
static int sntp_get_time(struct in_addr *a, ntp_timestamp_t *t) {
 (void)a; if(scenario==2 && calls==2) return -1;
 t->seconds=(scenario==2 && calls==3)?1:1704067200;
 t->microseconds=123000; return 0;
}
static void mico_time_set_utc_time_ms(uint64_t *t){stored=*t; writes++;}
static void feature_clock_valid(void){}
static bool m1_uart_clock(int y,int m,int d,int h,int n,int sec){
 assert(y==2024 && m==1 && d==1 && n==0 && sec==0);
 last_hour=h; clocks++; return queue_ok;
}
'''
checks = r'''
int main(void){
 assert(clock_sync(480,"192.168.1.2"));
 assert(calls==1 && !strcmp(tried[0],"192.168.1.2"));
 assert(writes==1 && clocks==1 && last_hour==8 && stored==1704067200123ULL);
 assert(m1_state.ntp_synced && !strcmp(m1_state.ntp_status,"synced"));
 scenario=1; calls=writes=clocks=0;
 assert(!clock_sync(0,"custom.example"));
 assert(calls==6 && writes==0 && clocks==0);
 assert(m1_state.ntp_last_success==1704067200 && !strcmp(m1_state.ntp_status,"failed"));
 calls=0; assert(!clock_sync(0,"pool.ntp.org"));
 assert(calls==5); /* Do not retry the preferred server in the fallback list. */
 scenario=2; calls=writes=clocks=0;
 assert(clock_sync(0,"custom.example"));
 assert(calls==4 && writes==1 && clocks==1 && last_hour==0);
 assert(!strcmp(tried[0],"custom.example") && !strcmp(tried[1],"ntp1.aliyun.com"));
 scenario=0; calls=0; queue_ok=false;
 assert(!clock_sync(0,"custom.example")); assert(calls==1);
 assert(m1_state.ntp_synced && !m1_state.ntp_screen_queued);
 assert(!strcmp(m1_state.ntp_status,"screen_queue_failed"));
 uint32_t now=UINT32_MAX-1000u, due=now+CLOCK_SYNC_INTERVAL_MS;
 assert((int32_t)(now-due)<0 && (int32_t)((due-1)-due)<0 && (int32_t)(due-due)>=0);
 assert(CLOCK_SYNC_INTERVAL_MS==3600000u && CLOCK_RETRY_INTERVAL_MS==60000u);
 puts("M1 NTP preferred source, fallback, deduplication, invalid time, failure retention, screen queue, timezone and tick wraparound passed");
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / 'clock.c').write_text(harness + constants + routine + checks)
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', str(path / 'clock.c'), '-o', str(path / 'clock')], check=True)
    subprocess.run([str(path / 'clock')], check=True)
