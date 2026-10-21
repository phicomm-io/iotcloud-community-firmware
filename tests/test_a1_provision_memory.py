"""Provisioning worker lifecycle and diagnostics under allocation failure."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

NETWORK = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#define UNUSED_PARAMETER(x) (void)(x)
#define custom_log(...) ((void)0)
#define kNoErr 0
#define Station 0
#define WATCHDOG_NETWORK 1
typedef int OSStatus;
typedef uintptr_t mico_thread_arg_t;
typedef struct { char ssid[33], wifi_password[65]; } a1_config_t;
typedef struct { bool is_connected; unsigned char ssid[32]; } LinkStatusTypeDef;
typedef struct { char ip[16]; } IPStatusTypedef;
static struct {
    bool ap_requested, ota_active, reboot_pending, wifi_connected, ap_active, scan_busy;
    char ip[16]; uint32_t reboot_at;
} a1_state;
static unsigned a1_lock, mqtt_calls, mqtt_failures, suspend_ap_calls, station_calls, ap_calls;
static unsigned loops, limit, ready_from, ready_until, blocked_until;
static bool saved_ssid, wrong_ssid, pending_reboot;
static unsigned ip_mode;
static uint32_t now, mqtt_times[8];
static jmp_buf done;
static void mico_rtos_lock_mutex(unsigned *m) { assert(!*m);*m=1; }
static void mico_rtos_unlock_mutex(unsigned *m) { assert(*m);*m=0; }
static uint32_t mico_rtos_get_time(void) { return now; }
static void a1_config_get(a1_config_t *c) {
    memset(c,0,sizeof(*c));if(saved_ssid)strcpy(c->ssid,"router");
}
static bool station_start(const a1_config_t *c) { (void)c;++station_calls;return true; }
static bool ap_start(void) { ++ap_calls;return true; }
static void micoWlanSuspendStation(void) { }
static void micoWlanSuspendSoftAP(void) { ++suspend_ap_calls; }
static bool wifi_trial_prepare(a1_config_t *c,uint32_t t) {
    (void)t;
    /* Model the existing credential trial retaining SoftAP until DHCP. */
    if(!saved_ssid && loops==ready_from) { saved_ssid=true;strcpy(c->ssid,"router");return true; }
    return false;
}
static int wifi_trial_finish(bool r,uint32_t t) { (void)r;(void)t;return 0; }
static int micoWlanGetLinkStatus(LinkStatusTypeDef *link) {
    link->is_connected=loops>=ready_from && loops<ready_until;
    strcpy((char *)link->ssid,wrong_ssid?"old-router":"router");return 0;
}
static int micoWlanGetIPStatus(IPStatusTypedef *ip,int mode) {
    assert(mode==Station);strcpy(ip->ip,ip_mode==1?"":ip_mode==2?"0.0.0.0":"192.168.1.2");return 0;
}
static void MicoSystemReboot(void) { assert(0); }
static void a1_config_flush(bool force) { assert(!force); }
static void watchdog_beat(int slot) { assert(slot==WATCHDOG_NETWORK); }
static bool watchdog_network_begin(void) { return true; }
static void watchdog_network_end(void) { }
static void feature_clock_tick(void) { }
static int a1_mqtt_start(void) {
    assert(a1_state.wifi_connected && loops>=ready_from && !a1_state.ota_active);
    assert(mqtt_calls<8);mqtt_times[mqtt_calls++]=now;
    if(mqtt_failures) { --mqtt_failures;return -1; }return 0;
}
static void mico_thread_msleep(unsigned ms) {
    assert(ms==500);now+=ms;
    ++loops;a1_state.ota_active=loops<blocked_until;
    if(loops==limit)longjmp(done,1);
}
WORKER
static void run(uint32_t initial,unsigned n,unsigned first_ready,unsigned end_ready,unsigned blocked,bool saved,unsigned failures) {
    memset(&a1_state,0,sizeof(a1_state));now=initial;loops=0;limit=n;
    ready_from=first_ready;ready_until=end_ready;blocked_until=blocked;saved_ssid=saved;
    mqtt_calls=suspend_ap_calls=station_calls=ap_calls=0;mqtt_failures=failures;
    a1_state.ota_active=blocked>0;
    a1_state.reboot_pending=pending_reboot;a1_state.reboot_at=initial+100000u;
    if(!setjmp(done))network_worker(0);
    assert(!a1_lock);
}
int main(void) {
    run(100,10,100,1000,0,false,0);assert(!mqtt_calls && ap_calls==1);
    run(100,10,3,1000,0,false,0);
    assert(mqtt_calls==1 && suspend_ap_calls==1 && mqtt_times[0]==1600);
    run(100,10,0,5,0,true,0);assert(mqtt_calls==1); /* Disconnect creates no second owner. */
    run(100,10,0,1000,3,true,0);assert(mqtt_calls==1 && mqtt_times[0]==1600);
    run(100,80,0,1000,0,true,1);
    assert(mqtt_calls==2 && mqtt_times[1]-mqtt_times[0]==30000);
    run(UINT32_MAX-10000u,80,0,1000,0,true,1);
    assert(mqtt_calls==2 && mqtt_times[1]-mqtt_times[0]==30000);
    run(100,80,0,5,0,true,1);assert(mqtt_calls==1); /* No allocation retries offline. */
    for(ip_mode=1;ip_mode<=2;ip_mode++) {
        run(100,10,0,1000,0,true,0);assert(!mqtt_calls);
    }
    ip_mode=0;wrong_ssid=true;
    run(100,10,0,1000,0,true,0);assert(!mqtt_calls);wrong_ssid=false;
    pending_reboot=true;
    run(100,10,0,1000,0,true,0);assert(!mqtt_calls);pending_reboot=false;
    puts("A1 MQTT deferred until DHCP; single owner, OTA gate, failed allocation retry/tick wrap: OK");
}
'''

MEMORY = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <stdlib.h>
typedef struct { int allocted_memory,free_memory,num_of_chunks; } micoMemInfo_t;
#include "common/memory_budget.h"
static void service_memory_snapshot(service_memory_state_t *s) { memset(s,0,sizeof(*s)); }
static micoMemInfo_t info;
static bool unavailable;
static char output[512];static unsigned status;
static const micoMemInfo_t *MicoGetMemoryInfo(void) { return unavailable?NULL:&info; }
static uint32_t mico_rtos_get_time(void) { return UINT32_MAX; }
static bool response(int fd,unsigned code,const char *type,const void *body,size_t n,bool gzip) {
    assert(fd==7 && (!strcmp(type,"application/json")||!strcmp(type,"text/plain")) && !gzip && n<sizeof(output));
    memcpy(output,body,n);output[n]=0;status=code;return true;
}
/* Neither diagnostic formatting nor its failure path may allocate. */
static void *no_alloc(size_t n) { (void)n;assert(0);return NULL; }
#define malloc no_alloc
#define calloc(...) no_alloc(0)
#define realloc(...) no_alloc(0)
#define json_object_new_object() no_alloc(0)
FUNCTION
int main(void) {
    info=(micoMemInfo_t){12345,0,9};memory_response(7);
    assert(status==200 && strstr(output,"\"free_memory\":0"));
    assert(strstr(output,"\"allocated_memory\":12345") && strstr(output,"\"free_memory_chunks\":9"));
    assert(strstr(output,"\"uptime_ms\":4294967295"));
    info=(micoMemInfo_t){INT_MAX,INT_MIN,INT_MAX};memory_response(7);
    assert(status==200 && strstr(output,"-2147483648"));
    unavailable=true;memory_response(7);assert(status==503 && !strcmp(output,"{\"code\":\"unavailable\"}"));
    puts("A1 memory diagnostics: no JSON/heap allocation, zero heap and unavailable stats: OK");
}
'''


class A1ProvisionMemoryTest(unittest.TestCase):
    PRODUCT = "A1"

    def run_c(self, source):
        with tempfile.TemporaryDirectory() as temp:
            probe = Path(temp) / 'probe.c'
            binary = Path(temp) / 'probe'
            probe.write_text(source)
            command = [os.environ.get('CC', 'cc'), '-std=c99', '-Wall', '-Wextra', '-Werror',
                       '-Wno-unused-function', '-I'+str(ROOT), str(probe), '-o', str(binary)]
            if os.environ.get('SANITIZE') == '1':
                command += ['-g', '-fno-omit-frame-pointer', '-fsanitize=address,undefined']
            subprocess.run(command, check=True)
            subprocess.run([str(binary)], check=True)

    def test_network_owns_deferred_mqtt_start(self):
        source = (ROOT / self.PRODUCT / 'network.c').read_text()
        worker = source[source.index('static void network_worker('):source.index(f'OSStatus {self.PRODUCT.lower()}_network_start(')]
        harness = NETWORK
        if self.PRODUCT == 'M1':
            harness = harness.replace('char ssid[33], wifi_password[65];', 'char ssid[33], wifi_password[65]; int timezone_minutes;')
            harness = harness.replace('char ip[16]; uint32_t reboot_at;', 'char ip[16]; uint32_t reboot_at, ntp_next_at;')
            harness = harness.replace('WORKER', r'''
#define CLOCK_SYNC_INTERVAL_MS 3600000u
#define CLOCK_RETRY_INTERVAL_MS 60000u
static void a1_ntp_host_get(char *host) { strcpy(host,"ntp.example"); }
static bool clock_sync(int timezone,const char *host) { (void)timezone;(void)host;return true; }
WORKER
''')
            harness = harness.replace('a1_', 'm1_').replace('A1 ', 'M1 ')
        self.run_c(harness.replace('WORKER', worker))


    def test_startup_reserves_http_and_control_before_network(self):
        source = (ROOT / 'A1/main.c').read_text()
        startup = source[source.index('    err = startup_result("features",'):source.index('    watchdog_startup_complete();')]
        self.run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#define kNoErr 0
#define custom_log(...) ((void)0)
typedef int OSStatus;
static unsigned stage,http,hardware,network;
static int startup_result(const char *s,int rc) { (void)s;return rc; }
static int feature_start(void) { return 0; }
static int a1_http_start(void) { http++;return stage==1?-1:0; }
static int a1_hardware_start(void) { assert(http);hardware++;return stage==2?-1:0; }
static int a1_network_start(void) { assert(http && hardware);network++;return stage==3?-1:0; }
static int a1_mqtt_start(void) { assert(0);return -1; }
static int startup(void) { OSStatus err;STARTUP return 0; }
int main(void) {
    for(stage=0;stage<4;stage++) {
        http=hardware=network=0;
        int rc=startup();assert(rc==(stage==1 || stage==3?-1:0));
        assert(http==1 && hardware==(stage!=1) && network==(stage!=1));
    }
    puts("A1 startup: provisioning/control stacks precede DHCP services, peripheral failure stays recoverable: OK");
}
'''.replace('STARTUP', startup))




class M1ProvisionMemoryTest(A1ProvisionMemoryTest):
    PRODUCT = "M1"

    def test_startup_reserves_http_and_control_before_network(self):
        source = (ROOT / 'M1/main.c').read_text()
        startup = source[source.index('    err = feature_start();'):source.index('    watchdog_startup_complete();')]
        self.run_c(r'''
#include <assert.h>
#include <stdio.h>
#define kNoErr 0
typedef int OSStatus;
static unsigned stage, features, uart, http, network;
static int feature_start(void) { features++;return stage==1?-1:0; }
static int m1_uart_start(void) { assert(features);uart++;return stage==2?-1:0; }
static int m1_http_start(void) { assert(uart);http++;return stage==3?-1:0; }
static int m1_network_start(void) { assert(http && uart);network++;return stage==4?-1:0; }
static int startup(void) { OSStatus err;STARTUP return 0; }
int main(void) {
    for(stage=0;stage<5;stage++) {
        features=uart=http=network=0;
        assert(startup()==(stage?-1:0));
        assert(features==1 && uart==(stage!=1));
        assert(http==(stage!=1 && stage!=2));
        assert(network==(stage!=1 && stage!=2 && stage!=3));
    }
    puts("M1 startup: local control and HTTP precede networking, failures stop later allocations: OK");
}
'''.replace('STARTUP', startup))
