"""TC1 native provisioning profile and retained AP entry regression."""
from pathlib import Path
import importlib.util
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class NativeSDK(unittest.TestCase):
    def test_profile_is_idempotent_and_keeps_wifi_and_console_sources(self):
        spec=importlib.util.spec_from_file_location('profile',ROOT/'tools/install_tc1_native_sdk.py')
        module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
        with tempfile.TemporaryDirectory() as td:
            sdk=Path(td);system=sdk/'MiCO/system/system.mk';core=sdk/'MiCO/core/core.mk'
            system.parent.mkdir(parents=True);core.parent.mkdir(parents=True)
            kept='$(NAME)_SOURCES += command_console/mico_cli.c easylink/system_easylink.c easylink/system_easylink_softap.c\n'
            system.write_text(kept+'$(NAME)_SOURCES += config_server/config_server_menu.c \\\n                   config_server/config_server.c\n')
            core.write_text('GLOBAL_LDFLAGS   += $$(CLIB_LDFLAGS_NANO_FLOAT)\n')
            module.install(sdk);before=(system.stat().st_mtime_ns,core.stat().st_mtime_ns)
            module.install(sdk);self.assertEqual(before,(system.stat().st_mtime_ns,core.stat().st_mtime_ns))
            self.assertIn(kept,system.read_text());self.assertNotIn('config_server.c',system.read_text())
            self.assertNotIn('NANO_FLOAT',core.read_text());self.assertIn('CLIB_LDFLAGS_NANO)',core.read_text())
            system.write_text('unexpected SDK layout')
            with self.assertRaises(RuntimeError):module.install(sdk)
        config=(ROOT/'TC1/mico_config.h').read_text()
        import re
        self.assertIsNone(re.search(r'^\s*#define\s+MICO_CONFIG_SERVER_ENABLE\b',config,re.M))
        self.assertIsNone(re.search(r'^\s*#define\s+MICO_SYSTEM_DISCOVERY_ENABLE\b',config,re.M))
        self.assertIsNone(re.search(r'^\s*#define\s+MICO_WLAN_CONNECTION_ENABLE\b',config,re.M))
        self.assertRegex(config,r'(?m)^#define MICO_CLI_ENABLE\b')

    def test_actual_button_recovery_ap_preserves_station_credentials(self):
        from tests.test_tc1_mqtt_stack import extract
        function=extract((ROOT/'TC1/user_wifi.c').read_text(),'void ApInit(')
        source=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
typedef int OSStatus;
enum {kNoErr=0,Soft_AP=1,DHCP_Server=2,WIFI_STATE_NOCONNECT=3};
typedef struct {char wifi_ssid[33],wifi_key[65],local_ip_addr[16],net_mask[16],gateway_ip_addr[16],dnsServer_ip_addr[16];int wifi_mode,dhcpMode,wifi_retry_interval;} network_InitTypeDef_st;
static struct {char ap_name[33],ap_key[33];} config;
#define user_config (&config)
static char str_mac[]="aabbccddeeff";
static char saved_ssid[]="saved-network",saved_key[]="saved-secret";
static bool wifi_config_ap_active;static int wifi_status,fail,calls,locks;
#define ZZ_AP_NAME "TC1_A1-AP-%s"
#define ZZ_AP_KEY ""
#define ZZ_AP_LOCAL_IP "192.168.0.1"
#define ZZ_AP_NET_MASK "255.255.255.0"
#define ZZ_AP_DNS_SERVER "192.168.0.1"
#define wifi_log(...) ((void)0)
#define TC1_STATUS_WRITE(...) do {__VA_ARGS__;}while(0)
static void tc1_status_lock(void){locks++;}static void tc1_status_unlock(void){locks--;}
static int micoWlanStart(network_InitTypeDef_st *n){
 calls++;assert(n->wifi_mode==Soft_AP&&n->dhcpMode==DHCP_Server);
 assert(!strcmp(n->local_ip_addr,"192.168.0.1")&&!strcmp(n->net_mask,"255.255.255.0"));
 assert(!strcmp(n->wifi_ssid,config.ap_name)&&!strcmp(n->wifi_key,config.ap_key));
 return fail?-1:kNoErr;
}
'''+function+r'''
int main(void){
 ApInit(true);assert(calls==1&&!locks&&wifi_config_ap_active&&wifi_status==WIFI_STATE_NOCONNECT);
 assert(!strcmp(config.ap_name,"TC1_A1-AP-DDEEFF")&&!config.ap_key[0]);
 strcpy(config.ap_name,"custom-ap");strcpy(config.ap_key,"adminfjr");ApInit(false);
 assert(calls==2&&!strcmp(config.ap_name,"custom-ap")&&!strcmp(config.ap_key,"adminfjr"));
 fail=1;ApInit(false);assert(calls==3&&!wifi_config_ap_active);
 assert(!strcmp(saved_ssid,"saved-network")&&!strcmp(saved_key,"saved-secret"));return 0;
}
'''
        with tempfile.TemporaryDirectory() as td:
            p=Path(td);(p/'test.c').write_text(source)
            subprocess.run(['cc','-std=c99','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-no-pie',str(p/'test.c'),'-o',str(p/'test')],check=True)
            subprocess.run([str(p/'test')],check=True)

if __name__=='__main__':unittest.main()
