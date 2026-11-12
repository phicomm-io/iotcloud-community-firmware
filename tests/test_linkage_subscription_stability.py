"""Peer edits must not reconnect the MQTT session that grants UDP access."""
from pathlib import Path
import subprocess, tempfile, unittest
from tests.test_runtime_mqtt_mp import function
ROOT=Path(__file__).resolve().parents[1]
class LinkageSubscriptionStability(unittest.TestCase):
 def test_peer_changes_keep_subscription_group_changes_do_not(self):
  source=(ROOT/'common/runtime.c').read_text()
  pre=r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#define FEATURE_DEVICE_A1
#define FEATURE_TOPIC_MAX 256
#define FEATURE_MQTT_A1_FILTER_MAX 3
#define FEATURE_MQTT_FILTER_ERROR 99
#define LOCAL_ID "aabbccddeeff"
#define mqtt_format snprintf
typedef struct {unsigned peer_count;char group[33];} feature_config_t;
typedef struct {int unused;} mqtt_account_t;
static feature_config_t config;
static mqtt_account_t mqtt_snapshot(void){mqtt_account_t a={0};return a;}
static void feature_config_get(feature_config_t*c){*c=config;}
static void mqtt_link_topic(mqtt_account_t*a,const char*b,const char*c,const char*d,const char*e,char*out,size_t n){(void)a;snprintf(out,n,"account/%s/%s/%s/%s",b,c,d,e);}
static void mqtt_aux(mqtt_account_t*a,const char*b,const char*c,char*out,size_t n){(void)a;snprintf(out,n,"account/%s/%s",b,c);}
static void root_for(feature_config_t*c,int unused,char*out){(void)unused;snprintf(out,FEATURE_TOPIC_MAX,"account/linkage/%s",c->group[0]?c->group:LOCAL_ID);}
'''
  code=pre+'\n'+function(source,'unsigned feature_mqtt_filters(')+'\n'+function(source,'bool feature_mqtt_filters_current(')+r'''
int main(void){char f[3][FEATURE_TOPIC_MAX];assert(feature_mqtt_filters(f,3)==3);
for(unsigned peers=0;peers<=4;peers++){config.peer_count=peers;assert(feature_mqtt_filters_current(f,3));}
config.peer_count=0;assert(feature_mqtt_filters_current(f,3));strcpy(config.group,"other-group");assert(!feature_mqtt_filters_current(f,3));
assert(feature_mqtt_filters(f,3)==3&&feature_mqtt_filters_current(f,3));char before[sizeof(f)];memcpy(before,f,sizeof(f));assert(feature_mqtt_filters(f,2)==FEATURE_MQTT_FILTER_ERROR&&!memcmp(before,f,sizeof(f)));return 0;}
'''
  with tempfile.TemporaryDirectory() as d:
   p=Path(d);(p/'test.c').write_text(code)
   subprocess.run(['cc','-std=c99','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie',str(p/'test.c'),'-o',str(p/'test')],check=True)
   subprocess.run([str(p/'test')],check=True)
