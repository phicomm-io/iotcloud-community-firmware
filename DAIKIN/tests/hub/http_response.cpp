/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <string>
#include <map>
#include <cstring>
#include <cassert>
#include <cstdio>
struct String:std::string {using std::string::string;String(const std::string&s):std::string(s){}bool startsWith(const char*p)const{return compare(0,strlen(p),p)==0;}bool reserve(size_t n){std::string::reserve(n);return true;}bool concat(const char*p,size_t n){append(p,n);return true;}};
#include "wire_mp.h"
using esp_err_t=int;static constexpr int ESP_OK=0,ESP_FAIL=-1;
struct httpd_req_t{std::string status,type,body;std::map<std::string,std::string> headers;};
static void httpd_resp_set_hdr(httpd_req_t*r,const char*k,const char*v){r->headers[k]=v;}
static void httpd_resp_set_type(httpd_req_t*r,const char*v){r->type=v;}
static void httpd_resp_set_status(httpd_req_t*r,const char*v){r->status=v;}
static int httpd_resp_send(httpd_req_t*r,const char*p,size_t n){r->body.assign(p,n);return ESP_OK;}
#include "response.inc"
static DynamicJsonDocument decode(const httpd_req_t&r){assert(r.type=="application/msgpack"&&r.headers.at("X-Content-Type-Options")=="nosniff");DynamicJsonDocument d(12288);assert(!deserializeMsgPack(d,r.body));return d;}
int main(){
 httpd_req_t r;assert(jsonResponse(&r,"200 OK","{\"value\":0,\"enabled\":false}")==ESP_OK);auto d=decode(r);assert(d.is<JsonObject>()&&d["value"]==0&&d["enabled"]==false);
 r={};assert(jsonResponse(&r,"200 OK","[{\"device_id\":\"02c400000001\"}]")==ESP_OK);d=decode(r);assert(d.is<JsonArray>()&&d.size()==1&&d[0]["device_id"]=="02c400000001");
 r={};assert(jsonResponse(&r,"200 OK","[]")==ESP_OK);d=decode(r);assert(d.is<JsonArray>()&&d.size()==0&&r.body==std::string(1,char(0x90)));
 // Exercise the dynamic-document path too, including binary NUL-safe writing.
 String large="[";for(unsigned i=0;i<80;i++){if(i)large+=",";large+="{\"value\":0}";}large+="]";r={};assert(jsonResponse(&r,"200 OK",large)==ESP_OK);d=decode(r);assert(d.is<JsonArray>()&&d.size()==80&&d[79]["value"]==0);
 r={};assert(jsonResponse(&r,"503 Service Unavailable","{\"code\":\"busy\"}")==ESP_OK);d=decode(r);assert(r.status=="503 Service Unavailable"&&d["code"]=="busy");
 // An already encoded response must be passed through unchanged.
 String encoded;serializeWireMP(d,encoded);r={};assert(jsonResponse(&r,"503 Service Unavailable",encoded)==ESP_OK);assert(r.body==encoded);
 r={};assert(jsonResponse(&r,"200 OK","[")==ESP_FAIL&&r.body.empty());
 puts("production HTTP MessagePack object/array/empty array/dynamic array/error/binary pass-through PASS");
}
