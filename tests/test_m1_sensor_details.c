#include <assert.h>
#include <stdio.h>
#include "../M1/sensor_details.h"
static const uint8_t raw[32]={0x42,0x4d,0,0x1c,0,13,0,19,0,19,0,13,0,19,0,19,8,0x94,2,0x7f,0,0x72,0,0,0,0,0,0,0x97,0,3,0x37};
static void metadata(uint8_t *p,unsigned generation,unsigned flags){
 memset(p,0,20);p[6]=32;p[8]=generation;p[11]=2;p[12]=0x74;p[13]=flags;p[14]=1;
}
static void sample(m1_details_t *d,unsigned at){
 uint8_t p[20],a[7];metadata(p,9,3);
 assert(!m1_details_reply(d,p,at));
 for(unsigned offset=0;offset<32;){
  m1_details_request(d,a);unsigned n=a[3];assert(a[2]==offset && a[5]==9);
  memset(p,0,20);p[6]=offset;p[7]=n;p[9]=9;memcpy(p+10,raw+offset,n);
  assert(!m1_details_reply(d,p,at));offset+=n;
 }
 metadata(p,9,3);assert(m1_details_reply(d,p,at));
}
int main(void){
 m1_details_t d={0};uint8_t p[20];assert(!m1_details_valid(&d,100));
 d.supported=true;sample(&d,100);
 assert(d.id==1 && m1_details_valid(&d,100));
 assert(m1_details_u16(d.frames[0].raw+16)==2196);
 assert(m1_details_valid(&d,5471));assert(!m1_details_valid(&d,5472));
 for(unsigned flags=0;flags<32;flags++){
  d.frames[0].flags=flags;assert(m1_details_valid(&d,100)==(flags==3));
 }
 d.frames[0].flags=3;d.frames[0].health=0;assert(!m1_details_valid(&d,100));d.frames[0].health=1;
 d.frames[0].raw[10]^=1;assert(!m1_details_valid(&d,100));d.frames[0].raw[10]^=1;
 d.supported=false;assert(!m1_details_valid(&d,100));d.supported=true;
 /* A different generation after the chunks cannot replace the old snapshot. */
 d.id=0;d.stage=2;d.pending=d.frames[0];metadata(p,10,3);
 assert(m1_details_reply(&d,p,100));assert(!m1_details_valid(&d,100));assert(d.frames[0].generation==9);
 d.id=0;d.stage=1;d.offset=0;d.pending.length=32;d.pending.generation=9;
 memset(p,0,20);p[7]=10; /* invalid chunk length */
 assert(m1_details_reply(&d,p,100));assert(d.stage==0);
 d.id=0;metadata(p,1,3);p[6]=33;assert(m1_details_reply(&d,p,100));
 d.id=0;memset(p,0,20);assert(m1_details_reply(&d,p,100));assert(!d.frames[0].received);
 d=(m1_details_t){.supported=true};sample(&d,UINT32_MAX-100);assert(m1_details_valid(&d,50));
 puts("sensor details: coherent frames, stale/wrap, health, flags, checksum and malformed replies OK");
}
