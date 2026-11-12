#include <assert.h>
#include <string.h>
#include "common/lan_wire.inc"
int main(void){
 lan_packet source={0},decoded;source.op=LAN_SAMPLE;strcpy(source.src,"aabbccddeeff");strcpy(source.dst,"112233445566");memset(source.token,0x81,16);source.boot=0xffffffff;source.id=42;
 source.sample.boot=source.boot;source.sample.sequence=99;source.sample.fields=31;source.sample.co2_supported=true;
 int32_t values[]={-12345,50000,25500,120000,850};memcpy(source.sample.value,values,sizeof(values));for(unsigned i=0;i<5;i++)source.sample.age_ms[i]=i*100;
 unsigned char storage[1200];ph_buf buffer;ph_writer w;assert(phw_init_fixed(&w,PH_WIRE_MSGPACK,&buffer,storage,sizeof(storage)));assert(lan_encode(&w,&source));size_t n=phw_finish_fixed(&w);assert(n&&lan_decode(storage,n,&decoded));assert(!memcmp(&source,&decoded,sizeof(source)));
 memset(storage,0,sizeof(storage));assert(decoded.sample.value[0]==-12345&&decoded.token[0]==0x81); /* independent copy */
 for(unsigned op=LAN_READ;op<=LAN_PONG;op++){if(op==LAN_SAMPLE)continue;source.op=op;assert(phw_init_fixed(&w,PH_WIRE_MSGPACK,&buffer,storage,sizeof(storage)));assert(lan_encode(&w,&source));n=phw_finish_fixed(&w);assert(lan_decode(storage,n,&decoded));assert(decoded.op==op);for(size_t i=0;i<n;i++)assert(!lan_decode(storage,i,&decoded));assert(!lan_decode(storage,n+1,&decoded));}
 source.op=LAN_SAMPLE;source.sample.value[4]=0;assert(phw_init_fixed(&w,PH_WIRE_MSGPACK,&buffer,storage,sizeof(storage)));assert(lan_encode(&w,&source));n=phw_finish_fixed(&w);assert(!lan_decode(storage,n,&decoded));
 return 0;
}
