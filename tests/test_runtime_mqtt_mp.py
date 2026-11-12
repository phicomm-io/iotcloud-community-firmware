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

    def test_native_virtual_commands_and_owned_group_updates(self):
        runtime = (ROOT / "common/runtime.c").read_text()
        fixture = r'''#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common/mqtt_account.h"
#include "common/typed_wire.h"
#include "common/ha_command.h"
#define FEATURE_DEVICE_A1 1
#define FEATURE_TOPIC_MAX MQTT_TOPIC_MAX
#define FEATURE_WIRE_MAX 1200
#define LOCAL_ID "aabbccddeeff"
typedef struct {char id[13];} member;
typedef struct {char group[33];unsigned member_count;member members[8];int value;} feature_config_t;
static feature_config_t current;
static bool coordinator=true;
static unsigned command_count,control_count,set_count,install_count;
static const char *domains[]={"binding","linkage","light","schedule","sleep","time"};
static const char *topic_domain(const char*d){return !strcmp(d,"light")?"light/config":d;}
static void feature_config_get(feature_config_t*c){*c=current;}
static bool feature_is_coordinator(void){return coordinator;}
static void root_for(const feature_config_t*c,unsigned i,char*out){(void)c;(void)i;strcpy(out,"phicomm/v2/u/group/G");}
static void feature_colon_mac(const char*in,char*out){(void)in;strcpy(out,"aa:bb:cc:dd:ee:ff");}
static bool feature_mac(const char*in,char*out){if(strlen(in)!=12||strspn(in,"0123456789abcdef")!=12)return false;strcpy(out,in);return true;}
static ph_buf *empty_wire(void){ph_writer w;assert(phw_init_capacity(&w,PH_WIRE_MSGPACK,32));phw_begin(&w,NULL,false);phw_end(&w);return phw_take(&w);}
ph_buf *service_command_mp(const void*d,size_t n){ph_value p,v;if(phv_parse(PH_WIRE_MSGPACK,d,n,&p)&&phv_get(&p,"action",&v)&&phv_equal_text(&v,"fan.set"))command_count++;return NULL;}
static bool feature_control_mp(const char*kind,const ph_value*p,const char**code){ph_value v;bool enabled;*code=NULL;assert(!strcmp(kind,"fan")||!strcmp(kind,"light"));assert(phv_get(p,"on",&v)||phv_get(p,"enabled",&v));assert(phv_bool(&v,&enabled)&&enabled);control_count++;return true;}
static ph_buf *domain_get_wire_mp(const char*d){(void)d;return empty_wire();}
static ph_buf *domain_set_wire_mp(const char*d,const ph_value*p,int*status){(void)d;ph_value v;int64_t n;*status=400;if(phv_get(p,"value",&v)&&phv_int(&v,&n)){current.value=n;set_count++;*status=200;}return empty_wire();}
static ph_buf *domain_config_wire_mp(const feature_config_t*c,const char*d,bool source){(void)c;(void)d;assert(source);return empty_wire();}
static bool feature_config_decode_mp(feature_config_t*c,const char*d,const ph_value*p){(void)d;ph_value v;int64_t n;if(!phv_get(p,"value",&v)||!phv_int(&v,&n))return false;c->value=n;return true;}
static bool install_config(feature_config_t*c){current=*c;install_count++;return true;}
static bool enqueue_mp(const char*t,const ph_buf*b,bool retained){assert(t&&b);(void)retained;return true;}
'''
        fixture += function(runtime, "static bool virtual_input(const char *t,const void *data,size_t n,bool retained){")
        fixture += r'''
static void input(const char*leaf,const void*d,size_t n,bool retained){char topic[512];snprintf(topic,sizeof(topic),"phicomm/v2/u/group/G/%s",leaf);assert(virtual_input(topic,d,n,retained));}
static size_t pack(const char*text,unsigned char*out){size_t n=ha_command_pack(text,strlen(text),out,1024);assert(n);return n;}
int main(void){
 strcpy(current.group,"G");current.member_count=1;strcpy(current.members[0].id,"001122334455");
 unsigned char wire[1024];const char*cmd="{\"action\":\"fan.set\",\"params\":{\"speed\":0}}";size_t n=pack(cmd,wire);assert(memchr(wire,0,n));
 input("command",wire,n,false);assert(command_count==1);
 input("command",cmd,strlen(cmd),false);assert(command_count==1);
 input("command",wire,n,true);assert(command_count==1);
 coordinator=false;input("command",wire,n,false);assert(command_count==1);coordinator=true;
 input("ha/command",cmd,strlen(cmd),false);assert(command_count==2);
 input("fan/set","ON",2,false);input("light/set","{\"enabled\":true}",16,false);assert(control_count==2);
 n=pack("{\"value\":0}",wire);input("linkage/set",wire,n,false);assert(set_count==1&&current.value==0);
 coordinator=false;input("linkage/set",wire,n,false);assert(set_count==1);coordinator=true;
 n=pack("{\"_source\":\"001122334455\",\"value\":7}",wire);input("linkage/state",wire,n,false);assert(install_count==1&&current.value==7);
 memset(wire,0,sizeof(wire));assert(current.value==7); /* transport bytes have no owner in persisted config */
 n=pack("{\"_source\":\"001122334455\",\"value\":7}",wire);input("linkage/state",wire,n,false);assert(install_count==1);
 n=pack("{\"_source\":\"112233445566\",\"value\":9}",wire);input("linkage/state",wire,n,false);assert(install_count==1&&current.value==7);
 n=pack("{\"_source\":\"001122334455\",\"value\":\"bad\"}",wire);input("linkage/state",wire,n,false);assert(install_count==1&&current.value==7);
 assert(!virtual_input("other/native",wire,n,false));
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            source, binary = Path(tmp) / "fixture.c", Path(tmp) / "fixture"
            source.write_text(fixture)
            subprocess.run(["cc", "-std=c99", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie", "-I", str(ROOT), "-I", str(ROOT / "common"), str(source), str(ROOT / "common/typed_wire.c"), str(ROOT / "common/wire_buffer.c"), "-lm", "-o", str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
