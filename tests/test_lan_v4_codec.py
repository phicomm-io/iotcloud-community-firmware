"""Exercise every LAN operation and reject extra, missing, renamed and mistyped fields."""
from tests.test_sensor_producers_mp import SensorProducers
import unittest

class LanV4Codec(unittest.TestCase):
 def test_strict_operation_schemas(self):
  SensorProducers().compile_run(r'''
#include "common/lan_wire_v4.inc"
#include <assert.h>
static void variant(const ph_value *root,int mode,unsigned selected){
 ph_writer w;assert(phw_init(&w,PH_WIRE_MSGPACK));phw_begin(&w,NULL,false);
 ph_iterator it;assert(phv_iter(root,&it));unsigned index=0;
 while(it.index<it.count){char key[128];ph_value value;assert(phv_next(&it,key,&value));
  if(index++==selected){
   if(mode==0)continue;
   if(mode==1){phw_value(&w,"unexpected",&value);continue;}
   if(mode==2){phw_null(&w,key);continue;}
  }
  phw_value(&w,key,&value);
 }
 if(mode==3)phw_null(&w,"unexpected");
 if(mode==4)phw_uint(&w,"id",2); /* duplicate required field */
 phw_end(&w);ph_buf *b=phw_take(&w);assert(b);lan_v4_packet decoded;
 assert(!lan_v4_decode(b->buf,b->len,&decoded));ph_buf_free(b);
}
int main(void){
 for(unsigned kind=0;kind<5;kind++){
  lan_v4_packet p={0};strcpy(p.src,"aabbccddeeff");strcpy(p.dst,"001122334455");
  memset(p.token,0x42,sizeof(p.token));p.boot=1;p.id=2;
  p.op=kind<4?(lan_v4_operation)(kind+1):LAN_V4_RESULT;
  if(p.op==LAN_V4_COMMAND){p.body="abc";p.body_length=3;}
  if(p.op==LAN_V4_RESULT){p.request_boot=7;strcpy(p.code,kind==4?"busy":"ok");
   if(kind!=4){p.body="abc";p.body_length=3;p.size=3;p.total=1;}}
  ph_writer w;assert(phw_init(&w,PH_WIRE_MSGPACK));assert(lan_v4_encode(&w,&p));
  ph_buf *b=phw_take(&w);assert(b);lan_v4_packet decoded;
  assert(lan_v4_decode(b->buf,b->len,&decoded)&&decoded.op==p.op);
  ph_value root;assert(phv_parse(PH_WIRE_MSGPACK,b->buf,b->len,&root));
  for(unsigned field=0;field<root.count;field++)for(int mode=0;mode<3;mode++)variant(&root,mode,field);
  variant(&root,3,0);variant(&root,4,0);
  for(size_t length=0;length<b->len;length++)assert(!lan_v4_decode(b->buf,length,&decoded));
  ph_buf_free(b);
 }
 return 0;
}
''')
