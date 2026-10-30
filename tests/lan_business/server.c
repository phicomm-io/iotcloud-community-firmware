#include "../service_mp/fixture.inc"
#define main legacy_auth_main
#include "../local_auth/test_local_auth.c"
#undef main
#include "common/lan_wire_v4.inc"
#include <openssl/sha.h>
static fixture f;static ph_auth_state auth;static lan_v4_server server;
static unsigned calls;static size_t result_size=3100;static bool reject,fail_result,fail_send;static unsigned char sent[1200];static size_t sent_size;
bool __real_ph_auth_context_valid(const ph_auth_context *,unsigned);
bool __wrap_ph_auth_context_valid(const ph_auth_context *c,unsigned scope){return __real_ph_auth_context_valid(c,scope);}
static uint32_t server_now(void*u){(void)u;return f.now;}
static bool authorize(void*u,const char*peer,const uint8_t token[16],ph_auth_context*c){
 (void)u;char text[33];static const char h[]="0123456789abcdef";for(unsigned i=0;i<16;i++){text[i*2]=h[token[i]>>4];text[i*2+1]=h[token[i]&15];}text[32]=0;
 return ph_auth_udp_check(&auth,peer,text,c);
}
static bool valid(void*u,const ph_auth_context*c){(void)u;return ph_auth_context_valid(c,PH_AUTH_SCOPE_CONTROL);}
static bool digest(void*u,const void*p,size_t n,uint8_t out[32]){(void)u;return SHA256(p,n,out)!=NULL;}
static ph_buf *dispatch(void*u,const void*p,size_t n,const ph_auth_context*c,bool*admitted){
 (void)u;(void)c;assert(n==3&&!memcmp(p,"one",3));
 assert(!server.response); /* old owner response released before new allocation */
 if(reject){*admitted=false;return NULL;}calls++;if(fail_result)return NULL;
 ph_buf *out=ph_buf_new(result_size+1);if(!out)return NULL;out->len=result_size;for(size_t i=0;i<result_size;i++)out->buf[i]=(char)(i%251);return out;
}
static bool send_packet(void*u,const char*ip,uint16_t port,const void*p,size_t n){(void)u;assert(!strcmp(ip,"127.0.0.1")&&port==51234&&n<=sizeof(sent));memcpy(sent,p,n);sent_size=n;return !fail_send;}
static lan_v4_ops ops={"001122334455",91,server_now,authorize,valid,digest,dispatch,send_packet,NULL};
static lan_v4_packet packet(const char*peer){
 lan_v4_packet p={0};p.op=LAN_V4_COMMAND;p.boot=11;p.id=1;strcpy(p.src,peer);strcpy(p.dst,ops.local);p.body="one";p.body_length=3;
 char key[33],token[33];assert(ph_auth_pair(&auth,"",peer,PH_AUTH_SCOPE_CONTROL,key));assert(ph_auth_pair_resume_boot(&auth,peer,key,p.boot,token));assert(ph_auth_token_decode(token,p.token));return p;
}
static void input(lan_v4_packet*p,const char*code){
 lan_v4_packet encode=*p;if(encode.op!=LAN_V4_COMMAND){encode.body=NULL;encode.body_length=0;}
 unsigned char wire[1201];ph_buf b;ph_writer w;assert(phw_init_fixed(&w,PH_WIRE_MSGPACK,&b,wire,sizeof(wire)));assert(lan_v4_encode(&w,&encode));size_t n=phw_finish_fixed(&w);assert(n);
 for(size_t i=0;i<n;i++){lan_v4_packet x;assert(!lan_v4_decode(wire,i,&x));}
 lan_v4_packet borrowed;assert(lan_v4_decode(wire,n,&borrowed));
 bool ok=lan_v4_server_input(&server,&ops,&borrowed,"127.0.0.1",51234,wire,sizeof(wire));assert(ok!=fail_send);
 memset(wire,0xa5,sizeof(wire));lan_v4_packet reply;assert(lan_v4_decode(sent,sent_size,&reply));if(strcmp(reply.code,code))fprintf(stderr,"id=%u op=%u part=%u got=%s expected=%s calls=%u\n",p->id,p->op,p->part,reply.code,code,calls);assert(!strcmp(reply.code,code));assert(reply.request_boot==p->boot&&reply.boot==ops.boot&&reply.id==p->id);
 if(!strcmp(code,"ok")){assert(reply.body_length&&reply.body_length<=768);for(size_t i=0;i<reply.body_length;i++)assert(((const unsigned char*)reply.body)[i]==(reply.part*768u+i)%251u);}
}
int main(void){
 setup(&f,&auth);lan_v4_packet a=packet("aabbccddeeff"),b=packet("112233445566");size_t initial=live_blocks;
 input(&a,"ok");assert(calls==1&&server.response&&server.response->len==3100);input(&a,"ok");assert(calls==1);
 input(&b,"busy");assert(calls==1);a.body="two";input(&a,"conflict");a.body="one";
 a.op=LAN_V4_PULL;for(unsigned i=0;i<5;i++){a.part=i;input(&a,"ok");}a.part=5;input(&a,"invalid_params");
 a.op=LAN_V4_ACK;a.part=0;input(&a,"acked");input(&a,"acked");assert(!server.response);
 a.op=LAN_V4_COMMAND;input(&a,"expired");assert(calls==1);a.id++;input(&a,"ok");assert(calls==2);
 a.id++;reject=true;input(&a,"busy");assert(calls==2&&!server.response);reject=false;input(&a,"ok");assert(calls==3);
 a.id++;fail_result=true;input(&a,"expired");assert(calls==4);fail_result=false;input(&a,"expired");assert(calls==4);
 a.id++;result_size=8193;input(&a,"too_large");assert(calls==5);result_size=8192;input(&a,"expired");assert(calls==5);
 a.id++;fail_send=true;input(&a,"ok");assert(calls==6&&server.response);fail_send=false;input(&a,"ok");assert(calls==6);
 f.now+=LAN_V4_RESPONSE_MS;lan_v4_server_expire(&server,&ops);assert(!server.response);input(&a,"expired");assert(calls==6);
 a.id++;a.boot++;input(&a,"conflict");a.boot--;
 a.token[0]^=1;input(&a,"auth_required");a.token[0]^=1;
 /* Four long-lived authenticated high-water marks, no response multiplication. */
 b.id=10;result_size=3100;input(&b,"ok");b.op=LAN_V4_ACK;input(&b,"acked");
 lan_v4_packet c=packet("223344556677"),d=packet("334455667788"),e=packet("445566778899");
 input(&c,"ok");c.op=LAN_V4_ACK;input(&c,"acked");input(&d,"ok");d.op=LAN_V4_ACK;input(&d,"acked");input(&e,"busy");
 assert(ph_auth_pair_remove(&auth,a.src));input(&e,"ok");lan_v4_server_release(&server);assert(live_blocks==initial);
 /* Millisecond wrap still expires a fixed five-second response. */
 f.now=UINT32_MAX-100;e=packet("445566778899");input(&e,"ok");f.now+=5000;lan_v4_server_expire(&server,&ops);assert(!server.response&&live_blocks==initial);
 e.id++;allocation_budget=0;input(&e,"expired");allocation_budget=-1;unsigned burned=calls;input(&e,"expired");assert(calls==burned);
 e.id++;test_heap_free=SERVICE_MEMORY_RESERVE;input(&e,"expired");test_heap_free=1024u*1024u;burned=calls;input(&e,"expired");assert(calls==burned&&live_blocks==initial);
 puts("v4 actual server: aliased IO/chunks/retry/conflict/ACK/5s/8192/slots/OOM tombstone PASS");return 0;
}
