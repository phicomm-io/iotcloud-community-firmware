#pragma once
#include <stdint.h>
#include <stddef.h>
namespace daikin {
inline bool checksum(const uint8_t *p, size_t n, size_t start=0) {
  uint8_t sum=0; for(size_t i=start;i<n;i++) sum+=p[i]; return sum==0;
}
inline bool co2(const uint8_t *p,size_t n,uint16_t &value) {
  if(n!=8||p[0]!=0x16||p[1]!=5||p[2]!=1||!checksum(p,n)) return false;
  value=(p[3]<<8)|p[4]; return value>=250&&value<=10000;
}
inline bool ch2o(const uint8_t *p,size_t n,uint16_t &value) {
  if(n!=9||p[0]!=0xff||p[1]!=0x86||!checksum(p,n,1)) return false;
  value=(p[2]<<8)|p[3]; return value<=5000;
}
struct Particulates { uint16_t pm25=0,pm10=0; };
inline bool pm(const uint8_t *p,size_t n,bool pm2005,Particulates &value) {
  const size_t o=pm2005?1:0;
  if(n!=12||p[o]!=0x16||p[o+2]!=0x80) return false;
  // Reference PM2005 has one leading bus byte; PM2105 starts at header.
  // The reference provides no verified checksum definition for these I2C frames.
  // Both modules return PM2.5 at P6/P7 and PM10 at P8/P9 in one reply.
  value.pm25=(p[o+5]<<8)|p[o+6];
  value.pm10=(p[o+7]<<8)|p[o+8];
  return true;
}
struct Sample {
  float value=0; uint32_t at=0; bool received=false;
  void set(float v,uint32_t now) {value=v;at=now;received=true;}
  bool valid(uint32_t now,uint32_t ttl=20000) const {return received&&uint32_t(now-at)<ttl;}
};
}
