#!/usr/bin/env python3
"""Compile actual overlaid native Wi-Fi events and SDK scan allocation paths."""
import argparse
from pathlib import Path
import subprocess


def function(text, signature):
    start = text.index(signature)
    brace = text.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end] + '\n'


WIFI_STUBS = r'''
#include <assert.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#define ADDLOGF_INFO(...) ((void)0)
#define PLATFORM_BEKEN 1
enum {WIFI_STA_CONNECTING,WIFI_STA_DISCONNECTED,WIFI_STA_AUTH_FAILED,
      WIFI_STA_CONNECTED,WIFI_AP_CONNECTED,WIFI_AP_FAILED};
static int g_bHasWiFiConnected,g_connectToWiFi,g_secondsElapsed,g_timeSinceLastPingReply;
static int g_newWiFiStatus,bSafeMode=1,DRV_SSDP_Active,DRV_MDNS_Active;
static unsigned char g_wifi_bssid[6];static int g_wifi_channel;
static void HAL_DisconnectFromWifi(void){}
static void HAL_GetWiFiBSSID(void *p){(void)p;}
static void HAL_GetWiFiChannel(void *p){(void)p;}
static const char *CFG_DeviceGroups_GetName(void){return "";}
static void ScheduleDriverStart(const char *s,int n){(void)s;(void)n;}
'''
WIFI_CASES = r'''
int main(void){
 Main_OnWiFiStatusChange(WIFI_AP_CONNECTED);assert(!g_bHasWiFiConnected);
 Main_OnWiFiStatusChange(WIFI_STA_CONNECTED);assert(g_bHasWiFiConnected);
 Main_OnWiFiStatusChange(WIFI_AP_FAILED);assert(g_bHasWiFiConnected);
 Main_OnWiFiStatusChange(WIFI_STA_DISCONNECTED);assert(!g_bHasWiFiConnected&&g_connectToWiFi==5);
 Main_OnWiFiStatusChange(WIFI_AP_CONNECTED);assert(!g_bHasWiFiConnected);
 Main_OnWiFiStatusChange(WIFI_STA_CONNECTING);assert(!g_bHasWiFiConnected&&g_connectToWiFi==120);
 Main_OnWiFiStatusChange(WIFI_AP_FAILED);assert(!g_bHasWiFiConnected);
 Main_OnWiFiStatusChange(WIFI_STA_AUTH_FAILED);assert(!g_bHasWiFiConnected&&g_connectToWiFi==5);
 puts("PASS: AP join/leave never changes STA connectivity or suppresses station retry");
}
'''
SDK_STUBS = r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
typedef uint8_t UINT8;typedef uint32_t UINT32;
#define MAX_BSS_LIST 64
#define ASSERT(x) assert(x)
struct sta_scan_res{unsigned char data[64];};
typedef struct sta_scan_res *SCAN_RST_ITEM_PTR;
typedef struct {UINT8 scanu_num;SCAN_RST_ITEM_PTR *res;} SCAN_RST_UPLOAD_T;
static SCAN_RST_UPLOAD_T *scan_rst_set_ptr;
static unsigned allocations,handled,released,forwarded;
static int fail_alloc,resultful_scan_cfm;
static void PH_BL_ScanCompleted(void *p,uint8_t n){(void)p;(void)n;}
static void supplicant_scan_callback(void *p,uint8_t n){(void)p;(void)n;}
static struct {void (*cb)(void *,uint8_t);} scan_cfm_cb={PH_BL_ScanCompleted};
static void PH_BL_ScanResult(const unsigned char *frame,unsigned bytes,int rssi){assert(frame&&bytes==4&&rssi==-55);++forwarded;}
static void *os_zalloc(size_t bytes){++allocations;return fail_alloc?NULL:calloc(1,bytes);}
static void sr_release_scan_results(SCAN_RST_UPLOAD_T *p){free(p);scan_rst_set_ptr=NULL;++released;}
static void mhdr_scanu_result_ind(SCAN_RST_UPLOAD_T *p,void *msg,unsigned n){assert(p&&p->res&&!p->scanu_num);(void)msg;(void)n;++handled;}
typedef struct {uint16_t length,framectrl,center_freq;uint8_t band,sta_idx,inst_nbr;int8_t rssi;uint32_t payload[1];} SCAN_IND_T,*SCAN_IND_PTR;
struct ke_msg{unsigned param_len;unsigned char param[2048];};
enum {SCANU_START_CFM,SCANU_RESULT_IND};
static SCAN_RST_UPLOAD_T *completion_table;
static void mhdr_scanu_start_cfm(void *msg,SCAN_RST_UPLOAD_T *p){(void)msg;completion_table=p;}
'''
SDK_CASES = r'''
int main(void){
 struct ke_msg msg={0};SCAN_IND_PTR ind=(SCAN_IND_PTR)msg.param;
 unsigned prefix=offsetof(SCAN_IND_T,payload);ind->length=4;ind->rssi=-55;msg.param_len=prefix+4;
 receive_result(&msg);assert(forwarded==1&&!allocations&&!handled&&!scan_rst_set_ptr);
 msg.param_len=prefix-1;receive_result(&msg);assert(forwarded==1);
 msg.param_len=prefix+3;receive_result(&msg);assert(forwarded==1);
 msg.param_len=prefix+4;
 scan_cfm_cb.cb=supplicant_scan_callback;
 fail_alloc=1;assert(!sr_malloc_result_item(500)&&!sr_malloc_shell());receive_result(&msg);assert(!handled&&!scan_rst_set_ptr);
 fail_alloc=0;receive_result(&msg);assert(handled==1&&scan_rst_set_ptr);
 receive_done(&msg);assert(completion_table==scan_rst_set_ptr);
 scan_cfm_cb.cb=PH_BL_ScanCompleted;receive_done(&msg);assert(!completion_table);
 scan_cfm_cb.cb=supplicant_scan_callback;
 resultful_scan_cfm=1;receive_result(&msg);assert(handled==2&&released==1&&scan_rst_set_ptr);
 sr_release_scan_results(scan_rst_set_ptr);
 unsigned before=allocations;assert(!sr_malloc_result_item(UINT32_MAX)&&allocations==before);
 void *item=sr_malloc_result_item(500);assert(item);free(item);
 item=sr_malloc_result_item(500);assert(item);free(item);
 item=sr_malloc_shell();assert(item);free(item);
 sr_release_scan_results(scan_rst_set_ptr);unsigned before_manual=allocations;
 scan_cfm_cb.cb=PH_BL_ScanCompleted;receive_result(&msg);assert(forwarded==2&&allocations==before_manual&&!scan_rst_set_ptr);
 puts("PASS: manual scans borrow bounded SDK frames without allocating an IE table; switching to supplicant preserves normal results/rollover; OOM shell skips safely and allocation overflow is rejected");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app-source', type=Path, required=True)
    parser.add_argument('--sdk-source', type=Path, required=True)
    parser.add_argument('--run-dir', type=Path, required=True)
    args = parser.parse_args()
    run = args.run_dir.resolve()
    run.mkdir(parents=True, exist_ok=True)
    native = (args.app_source/'src/user_main.c').read_text()
    sdk = (args.sdk_source/'platforms/bk7231n/bk7231n_os/beken378/func/rwnx_intf/rw_msg_rx.c').read_text()
    start = sdk.index('    case SCANU_RESULT_IND:')
    end = sdk.index('    case SM_CONNECT_IND:', start)
    done = sdk.index('    case SCANU_START_CFM:')
    cases = {
        'wifi-events': WIFI_STUBS + function(native, 'void Main_OnWiFiStatusChange(int code)') + WIFI_CASES,
        'sdk-scan': SDK_STUBS + function(sdk, 'static void ph_scan_result_view(struct ke_msg *msg)') +
            function(sdk, 'UINT8 *sr_malloc_result_item(UINT32 vies_len)') +
            function(sdk, 'UINT8 *sr_malloc_shell(void)') +
            'static void receive_result(struct ke_msg *rx_msg){switch(SCANU_RESULT_IND){\n' + sdk[start:end] + '}}\n' +
            'static void receive_done(struct ke_msg *rx_msg){switch(SCANU_START_CFM){\n' + sdk[done:start] + '}}\n' + SDK_CASES,
    }
    for name, text in cases.items():
        source = run/f'{name}.c'
        source.write_text(text)
        executable = run/name
        command = ['clang', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
                   '-fsanitize=address,undefined', str(source), '-o', str(executable)]
        with (run/f'{name}-compiler.log').open('w') as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
        with (run/f'{name}.log').open('w') as log:
            subprocess.run([str(executable)], stdout=log, stderr=subprocess.STDOUT, check=True, timeout=5)
        print((run/f'{name}.log').read_text(), end='')


if __name__ == '__main__':
    main()
