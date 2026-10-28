"""Exercise the included MQTT OTA adapter with exact binary payload lengths."""
import os, pathlib, shlex, subprocess, tempfile, unittest
ROOT=pathlib.Path(__file__).resolve().parents[1]
class MqttOta(unittest.TestCase):
 def test_native_control_and_retry(self):
  source=r'''
#include <assert.h>
#include <stdbool.h>
#include <string.h>
#include "common/typed_wire.h"
#define PH_MQTT_ERROR_APP_OTA_ENCODING 1
static int ph_mqtt_app_error(int e,int n){(void)e;return n;}
static unsigned launched,published,results;static bool reject,fail_publish,ready;
static char owned_url[500];static unsigned char last[384];static size_t last_n;
static bool ota_launch(const char *url){if(reject)return false;strcpy(owned_url,url);launched++;return true;}
static void ota_snapshot(char *state,char *error,unsigned *progress){strcpy(state,ready?"ready":"downloading");strcpy(error,"");*progress=ready?100:0;}
static int ota_publish(const char *leaf,const void *p,size_t n,bool retained){
 assert(n<=sizeof(last));ph_value root,v;assert(phv_parse(1,p,n,&root));assert(phv_get(&root,"request_id",&v));
 if(fail_publish){fail_publish=false;return -1;}memcpy(last,p,n);last_n=n;published++;
 if(!strcmp(leaf,"ota/result")){assert(!retained);results++;}else assert(retained);return 0;
}
#include "common/ota/mqtt_ota.inc"
static void command(const char *id,const char *url,bool retained){
 ph_writer w;assert(phw_init(&w,1));phw_begin(&w,0,false);phw_text(&w,"request_id",id);phw_text(&w,"url",url);phw_end(&w);
 ph_buf *b=phw_take(&w);assert(b);ota_command("owner",b->buf,b->len,retained);memset(b->buf,0,b->len);ph_buf_free(b);
}
int main(void){
 const char *url="http://example.test/a.signed";command("first",url,true);assert(!launched);
 const char json[]="{\"request_id\":\"json\",\"url\":\"x\"}";ota_command("owner",json,sizeof(json)-1,false);assert(!launched);
 command("bad id",url,false);assert(!launched);command("first",url,false);assert(launched==1&&!strcmp(owned_url,url));
 command("first",url,false);assert(launched==1);assert(!ota_service("other")&&!published);
 fail_publish=true;assert(ota_service("owner")==-1&&!published);assert(!ota_service("owner")&&published==1);
 assert(memchr(last,0,last_n));assert(!ota_service("owner")&&published==1);
 ready=true;assert(!ota_service("owner")&&published==3&&results==1);assert(!ota_service("owner")&&published==3);
 reject=true;command("reject",url,false);fail_publish=true;assert(ota_service("owner")==-1);assert(!ota_service("owner")&&results==2);
 ota_forget();assert(!ota_service("owner")&&results==2);return 0;
}
'''
  with tempfile.TemporaryDirectory() as d:
   p=pathlib.Path(d);(p/'case.c').write_text(source)
   cmd=shlex.split(os.environ.get('CC','cc'))+['-std=gnu99','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-I'+str(ROOT),str(p/'case.c'),str(ROOT/'common/typed_wire.c'),str(ROOT/'common/wire_buffer.c'),'-lm','-o',str(p/'test')]
   if os.environ.get('SANITIZE')=='1':cmd+=['-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie']
   r=subprocess.run(cmd,capture_output=True,text=True);self.assertEqual(r.returncode,0,r.stderr)
   r=subprocess.run([str(p/'test')],capture_output=True,text=True);self.assertEqual(r.returncode,0,r.stderr)
