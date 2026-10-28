"""Exercise extracted production MQTT adapters with real native MP/HA codecs.

Business persistence and transport are fakes; target compilation checks the
entire runtime separately. No JSON object library is linked.
"""
from pathlib import Path
import json
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def function(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    i, depth, quote, comment = opening + 1, 1, None, None
    while depth:
        c, nxt = source[i], source[i:i+2]
        if comment == "//":
            if c == "\n": comment = None
        elif comment == "/*":
            if nxt == "*/": comment = None; i += 1
        elif quote:
            if c == "\\": i += 1
            elif c == quote: quote = None
        elif nxt in ("//", "/*"): comment = nxt; i += 1
        elif c in ('"', "'"): quote = c
        elif c == "{": depth += 1
        elif c == "}": depth -= 1
        i += 1
    return source[start:i]


class RuntimeMqttMP(unittest.TestCase):
    def test_binary_boundaries_and_ha_discovery_retry(self):
        runtime = (ROOT / "common/runtime.c").read_text()
        fixture = r'''#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common/mqtt_account.h"
#include "common/typed_wire.h"
#include "common/ha_json.h"
#include "common/ha_command.h"
#define FEATURE_DEVICE_A1 1
#define FEATURE_TOPIC_MAX MQTT_TOPIC_MAX
#define FEATURE_WIRE_MAX 1200
#define FEATURE_STORAGE_PEERS 8
#define FEATURE_MAX_PEERS 4
#define LOCAL_ID "aabbccddeeff"
#define PH_MQTT_MORE 1
#define PH_MQTT_ERROR_APP_DISCOVERY_GRAPH -20
#define PH_LANG_ZH 0
#define PH_LANG_EN 1
static unsigned language;
static unsigned device_language_get(void){return language;}
static const char *ph_i18n(unsigned l,const char*z,const char*e){return l?e:z;}
typedef struct {char id[13],ip[16];} feature_member_t;
typedef struct {char group[33];feature_member_t members[8];char peer_ids[4][13];unsigned member_count,peer_count;} discovery_layout_t;
typedef struct {char name[64];} a1_config_t;
static int publish_error;static unsigned waits;static bool mqtt_discovery_deferred;
static void discovery_resource_wait(void){waits++;}
static void a1_config_get(a1_config_t*c){memset(c,1,sizeof(*c));c->name[63]=0;}
static mqtt_account_t account,feature_account;
static int namespace_lock;static uint32_t mqtt_generation=41;
static void mico_rtos_lock_mutex(int*p){(void)p;}
static void mico_rtos_unlock_mutex(int*p){(void)p;}
static unsigned queued;
static int ph_mqtt_enqueue_send_generation(const char*t,const void*b,size_t n,unsigned qos,bool retain,uint32_t generation){
 assert(t&&n==6&&((const unsigned char*)b)[5]==0&&qos==1&&!retain&&generation==41);queued++;return 0;
}
static mqtt_account_t mqtt_snapshot(void){return account;}
'''
        fixture += "\n".join(function(runtime, s) for s in (
            "static bool virtual_identity(", "static const char *virtual_device_name(",
            "static bool discovery_virtual_step(", "static int direct_wire_mp(", "static bool enqueue_mp("))
        fixture += r'''
static unsigned sends,fail_send;static size_t max_packet;
static int sender(const char*t,const void*b,size_t n,bool retain){
 assert(t&&b&&retain);sends++;
 if(fail_send)return -17;
 if(n){if(n>max_packet)max_packet=n;fwrite(b,1,n,stdout);putchar('\n');}
 return 0;
}
static unsigned binary_sends;
static int binary_sender(const char*t,const void*b,size_t n,bool retain){
 assert(!strcmp(t,"root/native"));assert(retain);assert(n==6);
 const unsigned char expected[]={0xde,0,1,0xa1,'x',0};assert(!memcmp(b,expected,n));binary_sends++;return 0;
}
int main(void){
 char user[64];memset(user,'U',63);user[63]=0;
 assert(mqtt_account_init(&account,1,"a1",user,LOCAL_ID));
 discovery_layout_t layout;memset(&layout,0,sizeof(layout));
 memset(layout.group,'G',32);layout.member_count=8;layout.peer_count=4;
 for(unsigned i=0;i<8;i++)snprintf(layout.members[i].id,13,"%012x",i+1);
 for(unsigned i=0;i<4;i++)snprintf(layout.peer_ids[i],13,"%012x",i+16);
 char root[512];snprintf(root,sizeof(root),"phicomm/v2/%s/group/0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",account.key);
 for(unsigned variant=0;variant<3;variant++){
 if(variant==1){memset(layout.group,1,32);}
 if(variant==2){memset(layout.group,0,sizeof(layout.group));}
 for(language=0;language<2;language++){
  unsigned cursor=0;fail_send=1;sends=0;publish_error=0;
  assert(!discovery_virtual_step(sender,&layout,root,false,&cursor));if(cursor!=0||publish_error!=-17)fprintf(stderr,"discovery cursor=%u error=%d sends=%u\n",cursor,publish_error,sends);assert(cursor==0&&publish_error==-17);
  fail_send=0;publish_error=0;
  for(unsigned turn=0;turn<80;turn++)if(discovery_virtual_step(sender,&layout,root,false,&cursor))break;
  assert(cursor==55);assert(!publish_error);assert(sends==40);
 }
 }
 unsigned cursor=0;assert(!discovery_virtual_step(sender,&layout,root,true,&cursor));assert(cursor==1);
 ph_writer w;assert(phw_init_capacity(&w,PH_WIRE_MSGPACK,32));phw_begin(&w,NULL,false);phw_uint(&w,"x",0);phw_end(&w);
 ph_buf *wire=phw_take(&w);feature_account=account;char destination[512];mqtt_aux(&account,"native","",destination,sizeof(destination));assert(enqueue_mp(destination,wire,false));assert(queued==1);assert(!enqueue_mp("phicomm/v2/other/native",wire,false));
 assert(!direct_wire_mp(binary_sender,"root","native",wire,true));assert(binary_sends==1);
 unsigned char output[256];const char json[]="{\"action\":\"fan.set\",\"params\":{\"on\":false,\"speed\":0}}";
 size_t n=ha_command_pack(json,sizeof(json)-1,output,sizeof(output));assert(n);ph_value body,params,v;
 assert(phv_parse(PH_WIRE_MSGPACK,output,n,&body));assert(phv_get(&body,"params",&params));assert(phv_get(&params,"speed",&v));int64_t speed=1;assert(phv_int(&v,&speed)&&speed==0);
 assert(!phv_parse(PH_WIRE_MSGPACK,json,sizeof(json)-1,&body));
 fprintf(stderr,"runtime MQTT MP: max HA packet=%zu, binary NUL preserved, retry cursor stable\n",max_packet);
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            source, binary = Path(tmp) / "fixture.c", Path(tmp) / "fixture"
            source.write_text(fixture)
            subprocess.run(["cc", "-std=c99", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie", "-I", str(ROOT), "-I", str(ROOT / "common"), str(source), str(ROOT / "common/typed_wire.c"), str(ROOT / "common/wire_buffer.c"), "-lm", "-o", str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            packets = [json.loads(line) for line in result.stdout.splitlines()]
            self.assertEqual(len(packets), 24)
            self.assertTrue(all(packet["command_topic"].endswith("/ha/command") for packet in packets))
            self.assertTrue(all(len(packet["availability"]) == 9 for packet in packets))
            self.assertTrue(all(packet["json_attributes_topic"].endswith("/ha/state") for packet in packets))
            self.assertIn("binary NUL preserved", result.stderr)
            print(result.stderr.strip())


if __name__ == "__main__":
    unittest.main()
