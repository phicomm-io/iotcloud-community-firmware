#include "frames.h"
#include <assert.h>
#include <stdio.h>
int main(){
 uint16_t value=0;
 uint8_t co2[]={0x16,5,1,3,0xe8,0,0,0xf9};
 assert(daikin::co2(co2,8,value)&&value==1000);
 co2[7]^=1;assert(!daikin::co2(co2,8,value));assert(!daikin::co2(co2,7,value));
 uint8_t ch2o[]={0xff,0x86,0,100,0,0,0,80,0};
 ch2o[8]=0-(ch2o[1]+ch2o[3]+ch2o[7]);
 assert(daikin::ch2o(ch2o,9,value)&&value==100);
 ch2o[8]^=1;assert(!daikin::ch2o(ch2o,9,value));
 ch2o[8]^=1;ch2o[1]=0x17;assert(!daikin::ch2o(ch2o,9,value));
 uint8_t pm2105[]={0x16,11,0x80,0,0,0,42,0,0,0,3,0};
 assert(daikin::pm(pm2105,12,false,value)&&value==42);
 assert(!daikin::pm(pm2105,12,true,value));
 uint8_t pm2005[]={0,0x16,11,0x80,0,0,0,43,0,0,0,3};
 assert(daikin::pm(pm2005,12,true,value)&&value==43);
 pm2005[3]=2;assert(!daikin::pm(pm2005,12,true,value));
 daikin::Sample s;assert(!s.valid(0));s.set(0,10);assert(s.valid(11));assert(!s.valid(20010));
 s.set(1,0xfffffff0);assert(s.valid(20));
 puts("PASS: sensor frame validation, variant offsets, freshness and wraparound");
}
