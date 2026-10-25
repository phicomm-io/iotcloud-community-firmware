#pragma once
#include "frames.h"

namespace daikin {
// Legacy CM1106-C UART (2019 V0.2): DF3/DF4 are reserved.
// Newer V0.6 flag decoding requires an explicitly verified module revision.
// An ACK proves command receipt, not the accuracy of the resulting measurement.
struct CM1106 {
  enum Calibration { Never, Queued, WaitingAck, Acknowledged, Timeout };
  Calibration calibration=Never;
  uint16_t raw=0,target=0;
  uint8_t status=0,reserved=0;
  bool statusFlagsSupported=false;
  bool received=false,warmed=false,attempted=false,awaitingMeasurement=false;
  uint32_t measuredAt=0,requestedAt=0,completedAt=0,errors=0,timeouts=0;
  uint8_t frame[8]={},used=0,waiting=0;
  uint32_t sentAt=0,nextQuery=0;
  bool pending() const {return calibration==Queued||calibration==WaitingAck;}
  bool fresh(uint32_t now) const {return received&&uint32_t(now-measuredAt)<20000;}
  bool valid(uint32_t now) const {return warmed&&fresh(now)&&(!statusFlagsSupported||!(status&0x7f))&&raw>=250&&raw<=10000&&!pending()&&!awaitingMeasurement;}
  uint32_t cooldown(uint32_t now) const {
    const uint32_t elapsed=now-requestedAt;
    return attempted&&elapsed<60000?(60000-elapsed+999)/1000:0;
  }
  bool canCalibrate(uint32_t now) const {
    return warmed&&fresh(now)&&(!statusFlagsSupported||!(status&0x0f))&&!pending()&&!cooldown(now);
  }
  bool request(uint16_t ppm,uint32_t now) {
    if(ppm<400||ppm>1500||!canCalibrate(now))return false;
    target=ppm;requestedAt=now;attempted=true;awaitingMeasurement=true;calibration=Queued;return true;
  }
  const char *result() const {
    switch(calibration){case Queued:return "queued";case WaitingAck:return "waiting_ack";
      case Acknowledged:return "acknowledged";case Timeout:return "timeout_unknown";default:return "never";}
  }
  void push(uint8_t b,uint32_t now) {
    if(!waiting)return;
    if(!used&&b!=0x16)return;
    frame[used++]=b;
    if(used==2&&frame[1]!=(waiting==3?1:5)){used=0;errors++;return;}
    const unsigned length=waiting==3?4:8;
    if(used<length)return;
    if(frame[2]!=waiting||!checksum(frame,length)){used=0;errors++;return;}
    if(waiting==3){calibration=Acknowledged;completedAt=now;nextQuery=now+2000;}
    else {raw=(frame[3]<<8)|frame[4];status=frame[5];reserved=frame[6];received=true;measuredAt=now;if(!pending())awaitingMeasurement=false;}
    used=0;waiting=0;
  }
  // Call from the UART owner loop only. No automatic calibration or retries.
  size_t poll(uint32_t now,uint8_t *out) {
    warmed=warmed||now>=180000;
    if(waiting&&uint32_t(now-sentAt)>(waiting==3?3000u:500u)){
      if(waiting==3){calibration=Timeout;completedAt=now;nextQuery=now+2000;}
      waiting=0;used=0;timeouts++;
    }
    if(waiting)return 0;
    if(calibration==Queued){
      out[0]=0x11;out[1]=3;out[2]=3;out[3]=target>>8;out[4]=target&255;
      out[5]=uint8_t(0-out[0]-out[1]-out[2]-out[3]-out[4]);
      calibration=WaitingAck;waiting=3;sentAt=now;return 6;
    }
    if(int32_t(now-nextQuery)<0)return 0;
    out[0]=0x11;out[1]=1;out[2]=1;out[3]=0xed;
    waiting=1;sentAt=now;nextQuery=now+5000;return 4;
  }
};
}
