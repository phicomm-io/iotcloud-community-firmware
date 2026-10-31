"""Production name handlers with bounded binary input and fallible persistence."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

from tests.mp_fixture_vectors import encode
from tests.test_http_json_budget import function

ROOT = Path(__file__).resolve().parents[1]


class DeviceNames(unittest.TestCase):
    def run_handler(self, source, entry, preamble, assertions, limit):
        values = [
            {"name": "客厅 Desk"}, {"name": "x" * limit},
            {}, {"name": ""}, {"name": "x" * (limit + 1)},
            {"name": 1}, {"name": True}, {"name": "ok", "password": "bad"},
            {"name": "ok", "language": "en"},
        ]
        arrays = "\n".join(
            "static const unsigned char input_%d[]={%s};" %
            (i, ",".join(map(str, encode(value)))) for i, value in enumerate(values))
        calls = "\n".join(
            'assert(!strcmp(call(input_%d,sizeof(input_%d)),"invalid_params"));' % (i, i)
            for i in range(2, len(values)))
        fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "common/typed_wire.h"
PREAMBLE
PRODUCTION
ARRAYS
static const char *call(const unsigned char *bytes,size_t n){
 ph_value params;assert(phv_parse(PH_WIRE_MSGPACK,bytes,n,&params));
 unsigned char output[256];ph_writer w;ph_buf storage;
 assert(phw_init_fixed(&w,PH_WIRE_MSGPACK,&storage,output,sizeof(output)));
 const char *code=NULL;assert(ENTRY(&params,&w,&code));assert(code);
 size_t length=phw_finish_fixed(&w);
 if(!strcmp(code,"accepted")){
  ph_value result,name;assert(length&&phv_parse(PH_WIRE_MSGPACK,output,length,&result));
  assert(phv_get(&result,"name",&name)&&phv_equal_text(&name,live.name));
 }
 return code;
}
int main(void){
 ASSERTIONS
 return 0;
}
'''.replace("PREAMBLE", preamble).replace("PRODUCTION", source).replace(
            "ARRAYS", arrays).replace("ENTRY", entry).replace(
            "ASSERTIONS", assertions.replace("INVALID_CALLS", calls))
        with tempfile.TemporaryDirectory(prefix="device-name-") as temporary:
            tmp = Path(temporary)
            (tmp / "test.c").write_text(fixture)
            command = [os.environ.get("CC", "cc"), "-std=gnu99", "-g", "-Wall",
                       "-Wextra", "-Werror", "-Wno-misleading-indentation",
                       "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie",
                       "-I" + str(ROOT), str(tmp / "test.c"),
                       str(ROOT / "common/typed_wire.c"), str(ROOT / "common/wire_buffer.c"),
                       "-lm", "-o", str(tmp / "test")]
            subprocess.run(command, check=True)
            subprocess.run([str(tmp / "test")], check=True, timeout=10)

    def test_mxchip_handler_only_changes_name_after_persistence(self):
        source = (ROOT / "common/service_product_mp.inc").read_text()
        production = "\n".join(function(source, signature) for signature in (
            "static void smp_empty(", "static __attribute__((noinline)) bool smp_name("))
        preamble = r'''
typedef struct {char name[64],wifi[33],password[33];unsigned revision;} CONFIG;
static CONFIG live={"original","preserved-wifi","preserved-password",7},disk;
static unsigned writes;static bool fail;
static void get_config(CONFIG *c){*c=live;}
static unsigned device_language_get(void){return 2;}
static bool save_config(const CONFIG *c,unsigned language){
 assert(language==2);writes++;if(fail)return false;disk=live=*c;return true;
}
#define GET_CONFIG get_config
#define SAVE_DEVICE_CONFIG save_config
'''
        assertions = r'''
 disk=live;
 assert(!strcmp(call(input_0,sizeof(input_0)),"accepted"));
 assert(!strcmp(live.name,"客厅 Desk")&&!strcmp(disk.name,live.name));
 assert(!strcmp(live.wifi,"preserved-wifi")&&!strcmp(live.password,"preserved-password"));
 assert(live.revision==7&&writes==1);
 CONFIG before=live;fail=true;
 assert(!strcmp(call(input_1,sizeof(input_1)),"storage_error"));
 assert(!memcmp(&before,&live,sizeof(live))&&!memcmp(&before,&disk,sizeof(disk)));
 unsigned previous=writes;
 INVALID_CALLS
 assert(writes==previous&&!memcmp(&before,&live,sizeof(live)));
 fail=false;assert(!strcmp(call(input_1,sizeof(input_1)),"accepted"));
 assert(strlen(live.name)==63&&!strcmp(disk.name,live.name));
'''
        self.run_handler(production, "smp_name", preamble, assertions, 63)

    def test_tc1_handler_noop_busy_and_flash_rollback(self):
        source = (ROOT / "TC1/service_mp.inc").read_text()
        production = "\n".join(function(source, signature) for signature in (
            "static bool keys(", "static bool strparam(",
            "static __attribute__((noinline)) bool tc1_name_mp("))
        preamble = r'''
#define maxNameLen 32
#define kNoErr 0
static struct {char name[32];} live={"original"},disk;
static struct {struct {char name[32];} micoSystemConfig;} system_names;
#define sys_config (&system_names)
static bool busy,locked,fail;static unsigned writes,refreshes;
static bool tc1_config_reserve(void){if(busy)return false;assert(!locked);locked=true;return true;}
static void tc1_config_release(void){assert(locked);locked=false;strcpy(live.name,sys_config->micoSystemConfig.name);}
static int mico_system_context_update(void *p){assert(p==sys_config&&locked);writes++;if(fail)return -1;strcpy(disk.name,sys_config->micoSystemConfig.name);return 0;}
static void UserMqttRefreshState(bool discovery){assert(discovery&&locked);refreshes++;}
'''
        assertions = r'''
 strcpy(sys_config->micoSystemConfig.name,live.name);disk=live;
 assert(!strcmp(call(input_0,sizeof(input_0)),"accepted"));
 assert(!strcmp(live.name,"客厅 Desk")&&!strcmp(disk.name,live.name));
 assert(writes==1&&refreshes==1&&!locked);
 assert(!strcmp(call(input_0,sizeof(input_0)),"accepted"));
 assert(writes==1&&refreshes==1&&!locked);
 busy=true;
 INVALID_CALLS
 assert(writes==1&&!locked);
 assert(!strcmp(call(input_1,sizeof(input_1)),"busy"));busy=false;
 fail=true;assert(!strcmp(call(input_1,sizeof(input_1)),"storage_error"));
 assert(!strcmp(live.name,"客厅 Desk")&&!strcmp(disk.name,live.name));
 assert(refreshes==1&&!locked);fail=false;
 assert(!strcmp(call(input_1,sizeof(input_1)),"accepted"));
 assert(strlen(live.name)==31&&!strcmp(disk.name,live.name)&&refreshes==2&&!locked);
'''
        self.run_handler(production, "tc1_name_mp", preamble, assertions, 31)
