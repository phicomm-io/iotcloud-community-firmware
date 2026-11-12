#define FEATURE_HOST
#include <arpa/inet.h>
#include <assert.h>
#include "../common/config_mp.inc"
#include "../common/engine.c"

static ph_buf *snapshot(feature_config_t *c,const char *domain){
 ph_writer w;assert(phw_init(&w,PH_WIRE_MSGPACK));
 assert(feature_config_encode_mp(c,domain,&w));
 ph_buf *b=phw_take(&w);assert(b);return b;
}
static ph_value parse(ph_buf *b){ph_value v;assert(phv_parse(PH_WIRE_MSGPACK,b->buf,b->len,&v));return v;}
static void roundtrip(const char *domain){
 feature_config_t source,dest;feature_defaults(&source);feature_defaults(&dest);
 source.revision=42;source.sleep_count=1;source.event_count=1;
 source.sleeps[0]=(feature_sleep_t){127,1,1,60,20,1,1320,420};
 source.automatic.night[0]=(feature_auto_night_t){10,40};
 source.events[0]=(feature_event_t){127,1,60,0,480};
 source.peer_count=1;strcpy(source.peers[0].id,"aabbccddeeff");strcpy(source.peers[0].ip,"10.0.0.2");
 source.peers[0].kind=FEATURE_M1;source.peers[0].timeout_ms=1000;source.peers[0].recovery_seconds=60;
 source.member_count=1;strcpy(source.members[0].id,"102030405060");strcpy(source.members[0].ip,"10.0.0.3");
 /* Existing pairing material is never leaked and survives replacing a peer. */
 dest.peer_count=1;strcpy(dest.peers[0].id,"aabbccddeeff");dest.peers[0].token[0]=123;dest.peers[0].confirmed=1;
 ph_buf *b=snapshot(&source,domain);ph_value v=parse(b);
 assert(feature_config_decode_mp(&dest,domain,&v));
 if(!strcmp(domain,"binding")){assert(dest.peers[0].token[0]==123);assert(dest.peers[0].confirmed==1);}
 if(!strcmp(domain,"sleep")){assert(dest.sleeps[0].max_speed==60);assert(dest.automatic.night[0].min_speed==10);}
 if(!strcmp(domain,"light"))assert(dest.light.speed==feature_light_speed(&source.light));
 ph_buf_free(b);
}
static void invalid(const char *domain,const char *field,double value,bool as_bool){
 feature_config_t c;feature_defaults(&c);ph_writer w;assert(phw_init(&w,PH_WIRE_MSGPACK));phw_begin(&w,NULL,false);
 if(as_bool)phw_bool(&w,field,value);else phw_number(&w,field,value);
 phw_end(&w);ph_buf *b=phw_take(&w);ph_value v=parse(b);assert(!feature_config_decode_mp(&c,domain,&v));ph_buf_free(b);
}
int main(void){
 const char *domains[]={"linkage","light","schedule","sleep","time","binding"};
 for(unsigned i=0;i<sizeof(domains)/sizeof(domains[0]);i++)roundtrip(domains[i]);
 invalid("linkage","fixed_speed",1.5,false);invalid("linkage","kp",1,true);invalid("linkage","metrics",8,false);
 invalid("time","timezone_minutes",841,false);invalid("light","segments",5,false);
 return 0;
}
