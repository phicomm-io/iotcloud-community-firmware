#include "cm1106.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
using daikin::CM1106;
static void measure(CM1106 &m,uint32_t now,uint8_t flags=0,uint16_t ppm=946){
 uint8_t f[]={0x16,5,1,uint8_t(ppm>>8),uint8_t(ppm),flags,0,0};
 for(unsigned i=0;i<7;i++)f[7]-=f[i];
 for(auto b:f)m.push(b,now);
}
int main(){
 uint8_t out[8];CM1106 m;
 assert(m.poll(0,out)==4);measure(m,1);assert(!m.valid(1));assert(!m.request(400,1));
 m.poll(180000,out);measure(m,180001);assert(m.valid(180001));
 assert(!m.request(399,180001)&&!m.request(1501,180001));
 assert(m.request(600,180001));assert(!m.valid(180001)&&!m.request(600,180002));
 assert(m.poll(180002,out)==6);
 const uint8_t expected[]={0x11,3,3,2,0x58,0x8f};assert(!memcmp(out,expected,6));
 const uint8_t bad[]={0x16,1,3,0xe7};for(auto b:bad)m.push(b,180003);
 assert(m.calibration==CM1106::WaitingAck&&m.errors==1);
 // A normal measurement cannot be mistaken for an acknowledgement.
 measure(m,180004);assert(m.calibration==CM1106::WaitingAck);
 const uint8_t ack[]={0x16,1,3,0xe6};for(auto b:ack)m.push(b,180005);
 assert(m.calibration==CM1106::Acknowledged&&!m.valid(180005));
 assert(!m.poll(182004,out));assert(m.poll(182005,out)==4);
 measure(m,182006,0,610);assert(m.valid(182006)&&m.raw==610);assert(!m.request(600,182006));
 assert(!m.valid(202006));
 // Each documented status bit invalidates consumer measurements, but retains raw diagnostics.
 for(unsigned bit=0;bit<7;bit++){
  CM1106 f;f.statusFlagsSupported=true;assert(f.poll(180000,out)==4);measure(f,180001,1<<bit);
  assert(f.received&&f.raw==946&&f.status==(1<<bit)&&!f.valid(180001));
  assert(f.canCalibrate(180001)==(bit>=4));
 }
 // Legacy reserved bytes must not be interpreted as V0.6 fault flags.
 CM1106 legacy;legacy.poll(180000,out);measure(legacy,180001,1);
 assert(legacy.valid(180001)&&legacy.canCalibrate(180001));
 assert(legacy.request(410,180001)&&legacy.poll(180002,out)==6);
 const uint8_t expected410[]={0x11,3,3,1,0x9a,0x4e};assert(!memcmp(out,expected410,6));
 CM1106 timeout;timeout.poll(180000,out);measure(timeout,180001);
 assert(timeout.request(400,180001));assert(timeout.poll(180002,out)==6);
 assert(timeout.poll(183003,out)==0&&timeout.calibration==CM1106::Timeout);
 assert(!strcmp(timeout.result(),"timeout_unknown"));
 // No automatic retry, even when the reply is lost or arrives late.
 for(auto b:ack)timeout.push(b,183004);
 assert(timeout.calibration==CM1106::Timeout);assert(timeout.poll(185003,out)==4);
 measure(timeout,185004);assert(timeout.valid(185004));
 CM1106 wrap;wrap.warmed=true;wrap.received=true;wrap.raw=420;wrap.measuredAt=0xfffffff0;
 wrap.attempted=true;wrap.requestedAt=0xfffffff0;
 assert(wrap.valid(20)&&wrap.cooldown(20)==60);assert(!wrap.canCalibrate(20));
 CM1106 partial;partial.poll(180000,out);partial.push(0x16,180001);partial.push(5,180002);
 assert(partial.poll(180501,out)==0&&partial.timeouts==1);
 assert(partial.poll(185000,out)==4);measure(partial,185001);assert(partial.valid(185001));
 puts("PASS CM1106: status flags, warmup, reference limits, ACK/checksum, stale data, cooldown, lost reply, recovery, wraparound");
}
