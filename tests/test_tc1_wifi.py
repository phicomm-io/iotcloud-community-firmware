"""Exercise the actual TC1 trial worker with a simulated WLAN and Flash."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class WifiTrial(unittest.TestCase):
    def test_verification_and_rollback(self):
        source = (ROOT / 'TC1/user_wifi.c').read_text()
        worker = source[source.index('static void WifiTrialWorker('):source.index('void WifiInit(')]
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
typedef int OSStatus;
typedef unsigned long mico_thread_arg_t;
typedef struct { char ssid[33], user_key[65], key[65]; unsigned user_keyLength,keyLength; unsigned char bssid[6]; } system_t;
static struct {system_t micoSystemConfig;} context;
#define sys_config (&context)
static struct {char ssid[33],key[65];bool active,dhcp;} wifi_trial;
static const char *wifi_trial_status;
typedef struct {char wifi_ssid[33],wifi_key[65];int wifi_mode,dhcpMode,wifi_retry_interval;} network_InitTypeDef_st;
typedef struct {bool is_connected;char ssid[33];} LinkStatusTypeDef;
typedef struct {char ip[16];} IPStatusTypedef;
enum {kNoErr=0,Station,DHCP_Client,MICO_APPLICATION_PRIORITY};
static uint32_t now;
static unsigned saves,rollbacks,aps,releases,mode;
static bool reserved;
static void (*queued)(mico_thread_arg_t);
static uint32_t mico_rtos_get_time(void){return now;}
static void mico_thread_msleep(unsigned n){now+=n;if(mode!=1)wifi_trial.dhcp=true;}
static void micoWlanSuspendStation(void){}
static OSStatus micoWlanStart(network_InitTypeDef_st *n){
 assert(!strcmp(n->wifi_ssid,"candidate"));
 assert(!strcmp(n->wifi_key,"new-password"));
 assert(!strcmp(context.micoSystemConfig.ssid,"saved"));
 assert(!strcmp(context.micoSystemConfig.user_key,"old-password"));
 assert(!saves);
 return mode==4 ? -1 : 0;
}
static OSStatus micoWlanGetLinkStatus(LinkStatusTypeDef *s){s->is_connected=true;strcpy(s->ssid,mode==2?"saved":"candidate");return 0;}
static OSStatus micoWlanGetIPStatus(IPStatusTypedef *s,int iface){(void)iface;strcpy(s->ip,"192.0.2.10");return 0;}
static OSStatus mico_system_context_update(void *c){(void)c;++saves;assert(!strcmp(context.micoSystemConfig.ssid,"candidate"));return mode==3?-1:0;}
static bool WifiHasSavedConfig(void){return context.micoSystemConfig.ssid[0]!=0;}
static OSStatus WifiStartStationFromSavedConfig(void){++rollbacks;assert(!strcmp(context.micoSystemConfig.ssid,"saved"));return 0;}
static void ApInit(bool defaults){(void)defaults;++aps;}
static bool tc1_config_reserve(void){if(reserved)return false;reserved=true;return true;}
static void tc1_config_release(void){assert(reserved);reserved=false;++releases;}
static void mico_rtos_delete_thread(void *t){(void)t;}
static OSStatus mico_rtos_create_thread(void *t,int priority,const char *name,void (*fn)(mico_thread_arg_t),int stack,mico_thread_arg_t arg){(void)t;(void)priority;(void)name;(void)stack;(void)arg;queued=fn;return mode==5?-1:0;}
''' + '#define TC1_STATUS_WRITE(...) do { __VA_ARGS__; } while(0)\n' + worker + r'''
int main(void){
 for(mode=0;mode<=5;++mode){
  memset(&context,0,sizeof(context));strcpy(context.micoSystemConfig.ssid,"saved");strcpy(context.micoSystemConfig.user_key,"old-password");
  now=UINT32_MAX-1000;saves=rollbacks=aps=releases=0;reserved=false;queued=NULL;
  assert(!WifiConnect("candidate","short"));assert(!reserved);
  bool accepted=WifiConnect("candidate","new-password");
  if(mode==5){assert(!accepted && !reserved && releases==1);continue;}
  assert(accepted && reserved && queued && !saves);
  assert(!WifiConnect("another","another-password"));
  queued(0);
  assert(!reserved && !wifi_trial.active && releases==1 && !wifi_trial.key[0] && !aps);
  if(mode==0){assert(saves==1 && !rollbacks && !strcmp(wifi_trial_status,"success"));assert(!strcmp(context.micoSystemConfig.ssid,"candidate"));}
  else {assert(rollbacks==1 && !strcmp(context.micoSystemConfig.user_key,"old-password"));assert(!strcmp(wifi_trial_status,mode==3?"storage_failure":"timeout_rollback"));assert(saves==(mode==3));}
 }
}
'''
        with tempfile.TemporaryDirectory() as directory:
            src = pathlib.Path(directory) / 'wifi.c'
            binary = pathlib.Path(directory) / 'wifi'
            src.write_text(harness)
            subprocess.run(['cc', '-std=gnu99', '-Wall', '-Wextra', '-Werror',
                            '-Wno-unused-parameter', str(src), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=5)


if __name__ == '__main__':
    unittest.main()
