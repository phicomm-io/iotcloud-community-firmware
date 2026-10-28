"""SDK-independent execution of the actual MessagePack-only core."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class MessagePackCore(unittest.TestCase):
    def run_c(self, body):
        with tempfile.TemporaryDirectory(prefix='msgpack-core-') as temporary:
            path = Path(temporary)
            source = path/'case.c'
            source.write_text(r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "common/typed_wire.h"
/* Real buffer allocator entry points are overridden for deterministic failures. */
static long budget=-1,blocks=0;
void *ph_wire_malloc(size_t size){if(!budget)return NULL;if(budget>0)--budget;void *p=malloc(size);if(p)blocks++;return p;}
void ph_wire_free(void *p){if(p){blocks--;free(p);}}
static bool parse(const void *p,size_t n,ph_value *v){return phv_parse(PH_WIRE_MSGPACK,p,n,v);}
''' + body)
            cmd = shlex.split(os.environ.get('CC', 'cc'))
            cmd += ['-std=gnu99','-Wall','-Wextra','-Werror',
                    '-Wno-misleading-indentation','-Wno-unused-function','-g',
                    '-I'+str(ROOT)]
            if os.environ.get('SANITIZE', '0') == '1':
                cmd += ['-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie']
            cmd += [str(source),str(ROOT/'common/typed_wire.c'),
                    str(ROOT/'common/wire_buffer.c'),'-lm','-o',str(path/'case')]
            built = subprocess.run(cmd,capture_output=True,text=True,timeout=30)
            self.assertEqual(built.returncode,0,built.stdout+built.stderr)
            ran = subprocess.run([str(path/'case')],capture_output=True,text=True,timeout=60)
            self.assertEqual(ran.returncode,0,ran.stdout+ran.stderr)

    def test_golden_scalars_binary_and_borrowed_fields(self):
        self.run_c(r'''
int main(void){
 const unsigned char golden[]={0xde,0,4,0xa1,'s',0xa2,0xc3,0xa9,0xa1,'b',0xc4,4,0,0xff,0xc0,0x7f,0xa1,'i',0xd1,0xfe,0xd4,0xa1,'n',0xcb,0x3f,0xf8,0,0,0,0,0,0};
 const unsigned char bytes[]={0,0xff,0xc0,0x7f};ph_writer w;assert(phw_init(&w,1));
 phw_begin(&w,NULL,false);phw_text(&w,"s","é");phw_bytes_value(&w,"b",bytes,sizeof(bytes));phw_int(&w,"i",-300);phw_number(&w,"n",1.5);phw_end(&w);
 ph_buf *out=phw_take(&w);assert(out&&out->len==sizeof(golden)&&!memcmp(out->buf,golden,sizeof(golden)));
 ph_value root,v;budget=0;assert(parse(out->buf,out->len,&root));budget=-1;assert(phv_keys(&root,"|s||b||i||n|"));
 assert(phv_get(&root,"s",&v)&&phv_equal_text(&v,"é"));char copied[8];assert(phv_text(&v,copied,sizeof(copied)));
 assert(v.text>=(unsigned char *)out->buf&&v.text<(unsigned char *)out->buf+out->len);
 assert(phv_get(&root,"b",&v));const void *borrowed;size_t n;assert(phv_bytes(&v,&borrowed,&n)&&n==4&&!memcmp(borrowed,bytes,n));assert(!phv_text(&v,copied,sizeof(copied)));
 int64_t integer;assert(phv_get(&root,"i",&v)&&phv_int(&v,&integer)&&integer==-300);
 double number;assert(phv_get(&root,"n",&v)&&phv_number(&v,&number)&&number==1.5);
 for(size_t cut=0;cut<out->len;cut++)assert(!parse(out->buf,cut,&v));ph_buf_free(out);assert(!blocks);
 /* Explicit nonminimal byte/string headers are accepted; payload remains borrowed. */
 const unsigned char wide[]={0x92,0xc6,0,0,0,2,0,0xff,0xda,0,1,'x'};
 assert(parse(wide,sizeof(wide),&root)&&phv_at(&root,0,&v)&&phv_bytes(&v,&borrowed,&n)&&n==2&&borrowed==wide+6);
 assert(phv_at(&root,1,&v)&&phv_equal_text(&v,"x"));
 const unsigned char nonminimal[]={0xcc,1};assert(parse(nonminimal,sizeof(nonminimal),&root));assert(phw_init(&w,1));phw_value_canonical(&w,NULL,&root);out=phw_take(&w);assert(out&&out->len==1&&out->buf[0]==1);ph_buf_free(out);
 assert(phw_init(&w,1));phw_bytes_value(&w,NULL,NULL,0);out=phw_take(&w);assert(out&&parse(out->buf,out->len,&v)&&phv_bytes(&v,&borrowed,&n)&&!n);ph_buf_free(out);assert(!blocks);return 0;
}
''')

    def test_reject_malformed_limits_and_duplicate_keys(self):
        self.run_c(r'''
static void bad(const unsigned char *bytes,size_t n){ph_value v;assert(!parse(bytes,n,&v));}
int main(void){
 const unsigned char duplicate[]={0x82,0xa1,'a',1,0xd9,1,'a',2};bad(duplicate,sizeof(duplicate));
 const unsigned char keybin[]={0x81,0xc4,1,'a',1};bad(keybin,sizeof(keybin));
 const unsigned char utf8[]={0xa2,0xc0,0xaf};bad(utf8,sizeof(utf8));
 const unsigned char nul[]={0xa1,0};bad(nul,sizeof(nul));
 const unsigned char surrogate[]={0xa3,0xed,0xa0,0x80};bad(surrogate,sizeof(surrogate));
 const unsigned char inf[]={0xcb,0x7f,0xf0,0,0,0,0,0,0};bad(inf,sizeof(inf));
 const unsigned char nan[]={0xca,0x7f,0xc0,0,0};bad(nan,sizeof(nan));
 const unsigned char unsafe[]={0xcf,0,0x20,0,0,0,0,0,0};bad(unsafe,sizeof(unsafe));
 const unsigned char unsafeneg[]={0xd3,0xff,0xe0,0,0,0,0,0,0};bad(unsafeneg,sizeof(unsafeneg));
 const unsigned char extensions[]={0xd4,0,0};bad(extensions,sizeof(extensions));
 const unsigned char huge[]={0xdd,0xff,0xff,0xff,0xff};bad(huge,sizeof(huge));
 const unsigned char truncated[]={0xc6,0,0,0,5,1};bad(truncated,sizeof(truncated));
 const unsigned char trailing[]={0xc0,0xc0};bad(trailing,sizeof(trailing));
 unsigned char depth[14];memset(depth,0x91,sizeof(depth));depth[13]=0xc0;bad(depth,sizeof(depth));
 assert(parse(depth+1,sizeof(depth)-1,&(ph_value){0}));
 unsigned char many[1027]={0xdc,4,0};memset(many+3,0xc0,1024);bad(many,sizeof(many));assert(parse(many+3,1,&(ph_value){0}));
 unsigned char oversized[8193];memset(oversized,0xc0,sizeof(oversized));bad(oversized,sizeof(oversized));
 const unsigned char textjson[]={'{','}'};bad(textjson,sizeof(textjson));
 ph_value root;assert(!phv_parse((ph_wire_format)0,depth,1,&root));assert(!phv_parse((ph_wire_format)2,depth,1,&root));
 const unsigned char whitelist[]={0x81,0xa0,1};assert(parse(whitelist,sizeof(whitelist),&root)&&!phv_keys(&root,"|action||params|"));
 const unsigned char joined[]={0x81,0xae,'a','c','t','i','o','n','|','|','p','a','r','a','m','s',1};assert(parse(joined,sizeof(joined),&root)&&!phv_keys(&root,"|action||params|"));
 uint32_t rng=17;unsigned char fuzz[256];for(unsigned i=0;i<15000;i++){for(unsigned j=0;j<sizeof(fuzz);j++){rng=rng*1664525+1013904223;fuzz[j]=(unsigned char)(rng>>24);}phv_parse(1,fuzz,i%256+1,&root);}
 assert(!blocks);return 0;
}
''')

    def test_owned_fixed_measure_oom_and_subtree_depth(self):
        self.run_c(r'''
static void emit(ph_writer *w){phw_begin(w,NULL,false);phw_text(w,"long","a repeated string forcing buffer growth");phw_begin(w,"array",true);phw_i32(w,NULL,INT32_MIN);phw_u32(w,NULL,UINT32_MAX);phw_bool(w,NULL,true);phw_null(w,NULL);phw_end(w);phw_end(w);}
int main(void){
 for(int fail=0;fail<12;fail++){budget=fail;ph_writer w;if(phw_init_capacity(&w,1,1)){emit(&w);ph_buf *b=phw_take(&w);if(b)ph_buf_free(b);}else phw_discard(&w);budget=-1;assert(!blocks);}
 ph_writer measure;ph_buf count;assert(phw_init_measure(&measure,1,&count));emit(&measure);size_t n=phw_finish_fixed(&measure);assert(n>50&&n<256);
 struct {unsigned char before[16],data[256],after[16];} fixed;memset(&fixed,0xa5,sizeof(fixed));ph_buf buffer;ph_writer w;
 assert(phw_init_fixed(&w,1,&buffer,fixed.data,n+1));emit(&w);assert(phw_finish_fixed(&w)==n);assert(parse(buffer.buf,buffer.len,&(ph_value){0}));
 memset(&fixed,0xa5,sizeof(fixed));assert(phw_init_fixed(&w,1,&buffer,fixed.data,4));emit(&w);assert(!phw_finish_fixed(&w));phw_discard(&w);
 for(unsigned i=0;i<16;i++)assert(fixed.before[i]==0xa5&&fixed.after[i]==0xa5);
 for(unsigned i=4;i<sizeof(fixed.data);i++)assert(fixed.data[i]==0xa5);
 for(unsigned layers=PHW_DEPTH-1;layers<=PHW_DEPTH;layers++){
  assert(phw_init(&w,1));for(unsigned d=0;d<layers;d++)phw_begin(&w,NULL,true);phw_uint(&w,NULL,7);for(unsigned d=0;d<layers;d++)phw_end(&w);
  ph_buf *input=phw_take(&w);assert(input);ph_value value;assert(parse(input->buf,input->len,&value));
  for(unsigned canonical=0;canonical<2;canonical++){
   assert(phw_init(&w,1));phw_begin(&w,NULL,false);if(canonical)phw_value_canonical(&w,"data",&value);else phw_value(&w,"data",&value);phw_end(&w);ph_buf *out=phw_take(&w);
   if(layers==PHW_DEPTH)assert(!out);else {assert(out&&parse(out->buf,out->len,&value));ph_buf_free(out);assert(parse(input->buf,input->len,&value));}
  }ph_buf_free(input);
 }
 assert(phw_init(&w,1));phw_number(&w,NULL,INFINITY);assert(!phw_take(&w));
 assert(phw_init(&w,1));phw_int(&w,NULL,INT64_MIN);assert(!phw_take(&w));
 assert(phw_init(&w,1));phw_uint(&w,NULL,UINT64_MAX);assert(!phw_take(&w));
 assert(!blocks);return 0;
}
''')

    def test_safe_integer_and_binary64_exact_roundtrip(self):
        self.run_c(r'''
int main(void){
 const int64_t numbers[]={0,1,-1,127,128,-128,-129,255,256,-32768,-32769,65535,65536,INT32_MIN,UINT32_MAX,INT64_C(9007199254740991),-INT64_C(9007199254740991)};
 for(unsigned i=0;i<sizeof(numbers)/sizeof(*numbers);i++){
  ph_writer w;assert(phw_init(&w,1));phw_int(&w,NULL,numbers[i]);ph_buf *b=phw_take(&w);assert(b);ph_value v;int64_t actual;assert(parse(b->buf,b->len,&v)&&phv_int(&v,&actual)&&actual==numbers[i]);ph_buf_free(b);
 }
 const double reals[]={0.0,-0.0,1.2345678901234567,1e-300,1e300,0x1p-1074};
 for(unsigned i=0;i<sizeof(reals)/sizeof(*reals);i++){
  ph_writer w;assert(phw_init(&w,1));phw_number(&w,NULL,reals[i]);ph_buf *b=phw_take(&w);ph_value v;double actual;assert(b&&parse(b->buf,b->len,&v)&&phv_number(&v,&actual)&&!memcmp(&actual,&reals[i],sizeof(actual)));ph_buf_free(b);
 }
 const unsigned char float32[]={0xca,0x3f,0xc0,0,0};ph_value v;double actual;assert(parse(float32,sizeof(float32),&v)&&phv_number(&v,&actual)&&actual==1.5);
 assert(!blocks);return 0;
}
''')


    def test_binary_length_boundaries_and_buffer_growth_transaction(self):
        self.run_c(r'''
int main(void){
 unsigned char payload[16384];for(unsigned i=0;i<sizeof(payload);i++)payload[i]=(unsigned char)i;
 const size_t lengths[]={0,1,255,256,4096};
 for(unsigned i=0;i<sizeof(lengths)/sizeof(*lengths);i++){
  ph_writer w;assert(phw_init_capacity(&w,1,1));phw_bytes_value(&w,NULL,payload,lengths[i]);ph_buf *b=phw_take(&w);assert(b);
  assert((unsigned char)b->buf[0]==(lengths[i]<=255?0xc4:0xc5));
  ph_value v;const void *p;size_t n;assert(parse(b->buf,b->len,&v)&&phv_bytes(&v,&p,&n)&&n==lengths[i]&&!memcmp(p,payload,n));
  ph_buf_free(b);
 }
 ph_writer w;assert(phw_init(&w,1));phw_bytes_value(&w,NULL,payload,sizeof(payload));assert(!phw_take(&w));
 ph_buf *buffer=ph_buf_new(4);assert(buffer);memcpy(buffer->buf,"abc",4);buffer->len=3;
 char *original=buffer->buf;budget=0;assert(!ph_buf_reserve(buffer,128));budget=-1;
 assert(buffer->buf==original&&buffer->cap==4&&buffer->len==3&&!memcmp(buffer->buf,"abc",4));
 assert(ph_buf_reserve(buffer,128)&&buffer->cap==128&&buffer->len==3&&!memcmp(buffer->buf,"abc",4));
 ph_buf_free(buffer);assert(!blocks);return 0;
}
''')


    def test_internal_cached_value_copy_without_reparse(self):
        self.run_c(r'''
int main(void){
 for(unsigned depth=PHW_DEPTH-1;depth<=PHW_DEPTH;depth++){
  ph_writer source;assert(phw_init(&source,1));for(unsigned d=0;d<depth;d++)phw_begin(&source,NULL,true);phw_null(&source,NULL);for(unsigned d=0;d<depth;d++)phw_end(&source);
  ph_buf *cached=phw_take(&source);assert(cached);ph_writer response;assert(phw_init_capacity(&response,1,256));
  budget=0;phw_begin(&response,NULL,false);phw_cached_value(&response,"data",cached,depth);phw_end(&response);budget=-1;
  ph_buf *result=phw_take(&response);
  if(depth==PHW_DEPTH)assert(!result);else {ph_value value;assert(result&&parse(result->buf,result->len,&value));ph_buf_free(result);}
  ph_buf_free(cached);
 }
 ph_writer w;assert(phw_init(&w,1));phw_cached_value(&w,NULL,w.wire,0);assert(!phw_take(&w));
 assert(phw_init(&w,1));phw_cached_value(&w,NULL,NULL,0);assert(!phw_take(&w));
 assert(!blocks);return 0;
}
''')

if __name__ == '__main__':
    unittest.main()
