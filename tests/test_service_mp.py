"""Execute the actual common dispatcher and allocator without a product SDK."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
class NativeService(unittest.TestCase):
 def test_dispatch_replay_cache_input_lifetime_and_recovery(self):
  with tempfile.TemporaryDirectory() as temporary:
   t=Path(temporary);(t/'generated').mkdir();(t/'generated/build_ota.h').write_text('')
   (t/'case.c').write_text(r'''
#include <assert.h>
#include <string.h>
#include "common/service.c"
static unsigned mutations,captures,caps;
static bool recovering;
/* Override production weak definitions from a separate translation unit below. */
static ph_buf *request(const char *action,const char *id,int value){
 ph_writer w;assert(phw_init(&w,1));phw_begin(&w,NULL,false);phw_text(&w,"action",action);
 if(id)phw_text(&w,"request_id",id);phw_begin(&w,"params",false);
 if(value>=0)phw_bool(&w,"full",value);phw_end(&w);phw_end(&w);return phw_take(&w);
}
static void code(ph_buf *out,const char *expected){ph_value root,v;assert(out&&phv_parse(1,out->buf,out->len,&root)&&phv_get(&root,"code",&v)&&phv_equal_text(&v,expected));}
static void object(ph_writer *w){phw_begin(w,NULL,false);phw_u32(w,"value",123);phw_end(w);}
void service_product_status_mp(ph_writer *w){captures++;object(w);}
void service_product_capabilities_mp(ph_writer *w,const char *const *extra,bool restricted){(void)extra;(void)restricted;caps++;object(w);}
void service_product_info_mp(ph_writer *w){object(w);}
bool service_product_mp(const char *a,const ph_value *p,ph_writer *w,const char **out){(void)p;if(strcmp(a,"system.reset")&&strcmp(a,"fan.set"))return false;mutations++;*out="accepted";object(w);return true;}
bool automation_mp(const char *a,const ph_value *p,ph_writer *w,const char **out){(void)a;(void)p;(void)w;(void)out;return false;}
bool service_product_ota_start(const char *url){(void)url;return true;}
int main(void){
 service_init();ph_buf *a=service_query_mp("status.get"),*b=service_query_mp("status.get");code(a,"ok");code(b,"ok");assert(captures==1);assert(a->buf!=b->buf);ph_buf_free(a);ph_buf_free(b);
 service_cache_changed(SERVICE_CACHE_STATUS);a=service_query_mp("status.get");code(a,"ok");assert(captures==2);ph_buf_free(a);
 a=service_query_mp("capabilities.get");b=service_query_mp("capabilities.get");assert(caps==1);ph_buf_free(a);ph_buf_free(b);
 ph_buf *q=request("fan.set","once",-1);a=service_command_mp(q->buf,q->len);code(a,"accepted");assert(mutations==1);b=service_command_mp(q->buf,q->len);code(b,"accepted");assert(mutations==1);ph_buf_free(b);
 memset(q->buf,0,q->len);code(a,"accepted");ph_buf_free(q);ph_buf_free(a);
 q=request("system.reset","once",1);a=service_command_mp(q->buf,q->len);code(a,"conflict");assert(mutations==1);ph_buf_free(q);ph_buf_free(a);
 const char json[]="{\"action\":\"fan.set\",\"params\":{}}";a=service_command_mp(json,sizeof(json)-1);code(a,"invalid_params");ph_buf_free(a);assert(mutations==1);
 a=service_query_mp("unsupported.get");code(a,"unsupported");ph_buf_free(a);
 a=service_query_mp("logs.get");code(a,"ok");ph_buf_free(a);
 return 0;
}
''')
   cmd=[os.environ.get('CC','cc'),'-std=gnu99','-Wall','-Wextra','-Werror','-Wno-deprecated-declarations','-Wno-misleading-indentation','-Wno-unused-function','-Wno-unused-variable','-g','-I'+str(ROOT/'tests/service_mp'),'-I'+str(t),'-I'+str(ROOT)]
   if os.environ.get('SANITIZE')=='1':cmd+=['-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie']
   cmd += [str(t/'case.c'),str(ROOT/'common/typed_wire.c'),str(ROOT/'common/wire_buffer.c'),'-lcrypto','-lm','-o',str(t/'case')]
   built=subprocess.run(cmd,capture_output=True,text=True,timeout=30);self.assertEqual(built.returncode,0,built.stderr)
   ran=subprocess.run([str(t/'case')],capture_output=True,text=True,timeout=30);self.assertEqual(ran.returncode,0,ran.stderr)
if __name__=='__main__':unittest.main()
