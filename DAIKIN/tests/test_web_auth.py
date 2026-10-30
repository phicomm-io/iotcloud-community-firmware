"""Actual DAIKIN token handlers with real MessagePack parsing on the host."""
from pathlib import Path
import subprocess,tempfile,unittest
from DAIKIN.tests.arduinojson import codec_source
ROOT=Path(__file__).resolve().parents[1]
class Auth(unittest.TestCase):
 def test_sessions(self):
  with tempfile.TemporaryDirectory() as directory:
   temp=Path(directory)
   http=(ROOT/'src/http.inc').read_text().split('// HTTP handlers run',1)[0]
   (temp/'auth.inc').write_text(http)
   (temp/'test.cpp').write_text(r'''
#include <ArduinoJson.h>
#include <string>
#include <map>
#include <cstring>
#include <cassert>
#include <cstdio>
#include "local_auth.h"
extern "C" {
#include "typed_wire.h"
}
class String:public std::string {
public:using std::string::string;using std::string::operator=;
 bool startsWith(const char *s)const{return compare(0,strlen(s),s)==0;}
 bool reserve(size_t n){std::string::reserve(n);return true;}
 bool concat(const char *p,size_t n){append(p,n);return true;}
 size_t write(uint8_t c){push_back(char(c));return 1;}
 size_t write(const uint8_t *p,size_t n){append((const char*)p,n);return n;}
};

#include "wire_mp.h"
namespace ArduinoJson {template<> struct Converter<String>{static void toJson(const String &s,JsonVariant out){out.set(static_cast<const std::string&>(s));}};}
using esp_err_t=int;
static const int ESP_OK=0,ESP_FAIL=-1;
static char webPassword[33]="";static int authMux=0;
#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux) ((void)(mux))
static uint32_t clockMs=100,randomValue=0;
static uint32_t millis(){return clockMs;}

struct httpd_req_t{int content_len=0,offset=0;std::string body;std::map<std::string,std::string> headers;};
static std::string response,type,status;
static int httpd_resp_set_hdr(httpd_req_t*,const char*,const char*){return 0;}
static int httpd_resp_set_status(httpd_req_t*,const char *s){status=s;return 0;}
static int httpd_resp_set_type(httpd_req_t*,const char *s){type=s;return 0;}
static int httpd_resp_send(httpd_req_t*,const char *p,size_t n){response.assign(p,n);return 0;}
static int httpd_req_get_hdr_value_str(httpd_req_t *req,const char *name,char *out,size_t n){auto it=req->headers.find(name);if(it==req->headers.end()||it->second.size()>=n)return -1;memcpy(out,it->second.c_str(),it->second.size()+1);return 0;}
static int httpd_req_recv(httpd_req_t *req,char *out,int n){int count=std::min(n,req->content_len-req->offset);memcpy(out,req->body.data()+req->offset,count);req->offset+=count;return count;}
static ph_auth_state localAuth;static bool localAuthReady=true,configRecovery=false;
#include "auth.inc"
static httpd_req_t login(const char *password){httpd_req_t req;DynamicJsonDocument doc(512);doc["password"]=password;serializeMsgPack(doc,req.body);req.content_len=req.body.size();req.headers["Content-Type"]="application/msgpack";loginHandler(&req);assert(type=="application/msgpack");return req;}
static std::string token(){DynamicJsonDocument doc(512);assert(!deserializeMsgPack(doc,response));return doc["data"]["token"]|"";}
int main(){
 (void)currentWebPassword;(void)millis;
 ph_auth_ops ops={};ops.now=[](void*){return clockMs;};ops.password=[](void*,char out[33]){strcpy(out,webPassword);return true;};ops.random=[](void*,uint8_t*out,size_t n){for(size_t i=0;i<n;i++)out[i]=uint8_t(++randomValue);return true;};ph_auth_store store;ph_auth_store_empty(&store);assert(ph_auth_init(&localAuth,&ops,&store));
 login("");assert(status=="200 OK");auto first=token();assert(first.size()==32);httpd_req_t auth;auth.headers["Authorization"]="Bearer "+first;assert(authorized(&auth));
 for(unsigned i=0;i<3;i++){login("");assert(status=="200 OK");}
 login("");assert(status=="503 Service Unavailable");assert(authorized(&auth));
 logoutHandler(&auth);assert(status=="200 OK");assert(!authorized(&auth));
 login("");assert(status=="200 OK");auth.headers["Authorization"]="Bearer "+token();
 strcpy(webPassword,"adminfjr");assert(!authorized(&auth));login("");assert(status=="401 Unauthorized");
 login("adminfjr");assert(status=="200 OK");auth.headers["Authorization"]="Bearer "+token();assert(authorized(&auth));
 clockMs+=1800000;assert(!authorized(&auth));
 login("adminfjr");assert(status=="200 OK");auth.headers["Authorization"]="Bearer "+token();
 for(unsigned i=0;i<47;i++){clockMs+=1799999;assert(authorized(&auth));}clockMs+=1799999;assert(authorized(&auth));clockMs+=100;assert(!authorized(&auth));
 auto req=login("adminfjr");req.offset=0;req.body="{\"password\":\"adminfjr\"}";req.content_len=req.body.size();loginHandler(&req);assert(status=="400 Bad Request");
 req.headers["Authorization"]="Basic YWRtaW46YWRtaW5manI=";assert(!authorized(&req));
}
''')
   objects=[]
   for name in ('local_auth','typed_wire','wire_buffer'):
    obj=temp/(name+'.o');subprocess.run(['cc','-fsanitize=address,undefined','-fno-pie','-c',str(ROOT.parent/'common'/f'{name}.c'),'-o',str(obj)],check=True);objects.append(str(obj))
   subprocess.run(['c++','-std=c++11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(temp),'-I'+str(ROOT.parent/'common'),'-I'+str(ROOT/'include'),'-I'+str(codec_source()),str(temp/'test.cpp'),*objects,'-o',str(temp/'test')],check=True)
   subprocess.run([str(temp/'test')],check=True)
