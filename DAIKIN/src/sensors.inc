static bool writeI2c(TwoWire &bus,uint8_t address,const uint8_t *data,size_t n){
  bus.beginTransmission(address);bus.write(data,n);return bus.endTransmission()==0;
}
static bool readI2c(TwoWire &bus,uint8_t address,uint8_t *data,size_t n){
  if(bus.requestFrom(address,(uint8_t)n)!=(int)n){while(bus.available())bus.read();return false;}
  for(size_t i=0;i<n;i++)data[i]=bus.read();return true;
}
static uint8_t crc8(const uint8_t *p){uint8_t c=0xff;for(int i=0;i<2;i++){c^=p[i];for(int b=0;b<8;b++)c=(c&128)?(c<<1)^0x31:c<<1;}return c;}
static void pollSensors(uint32_t now){
  static bool warmedUp=false;warmedUp=warmedUp||now>=180000;
  // Assemble UART replies across loop iterations, with header resynchronization.
  while(co2Uart.available()){
    uint8_t b=co2Uart.read();if(!co2Waiting)continue;
    if(co2Used==0&&b!=0x16)continue;
    co2Frame[co2Used++]=b;
    if(co2Used==8){uint16_t v;if(daikin::co2(co2Frame,8,v)&&warmedUp)readings[1].set(v,now);co2Used=0;co2Waiting=false;}
  }
  while(hchoUart.available()){
    uint8_t b=hchoUart.read();if(!hchoWaiting)continue;
    if(hchoUsed==0&&b!=0xff)continue;
    hchoFrame[hchoUsed++]=b;
    if(hchoUsed==9){uint16_t v;if(daikin::ch2o(hchoFrame,9,v)){hchoPresent=true;if(warmedUp)readings[5].set(v/1000.0f,now);}hchoUsed=0;hchoWaiting=false;}
  }
  if(co2Waiting&&uint32_t(now-co2Sent)>500){co2Used=0;co2Waiting=false;}
  if(hchoWaiting&&uint32_t(now-hchoSent)>500){hchoUsed=0;hchoWaiting=false;}
  if(due(now,nextSample)){
    nextSample=now+5000;
    uint8_t p[12];uint16_t v;
    if(readI2c(pmBus,0x28,p,12)){
      if(daikin::pm(p,12,false,v)){pmModel="PM2105";readings[0].set(v,now);}
      else if(daikin::pm(p,12,true,v)){pmModel="PM2005";readings[0].set(v,now);}
    }
    // HDC1080 temperature+humidity acquisition; bounded 15 ms conversion.
    const uint8_t reg[]={0};
    if(writeI2c(thBus,0x40,reg,1)){
      delay(15);uint8_t th[4];
      if(readI2c(thBus,0x40,th,4)){
        float t=((th[0]<<8)|th[1])*165.0f/65536.0f-40;
        float h=((th[2]<<8)|th[3])*100.0f/65536.0f;
        if(t>=-40&&t<=125&&h>=0&&h<=100){readings[2].set(t,now);readings[3].set(h,now);}
      }
    }
    const uint8_t co2Query[]={0x11,1,1,0xed};
    while(co2Uart.available())co2Uart.read();
    co2Uart.write(co2Query,4);co2Sent=now;co2Waiting=true;co2Used=0;
    const uint8_t hchoQuery[]={0xff,1,0x86,0,0,0,0,0,0x79};
    while(hchoUart.available())hchoUart.read();
    hchoUart.write(hchoQuery,9);hchoSent=now;hchoWaiting=true;hchoUsed=0;
  }
  // SGP30 requires a measurement every second. Report native TVOC ppb, not
  // the reference's heuristic mass concentration multiplier.
  if(due(now,nextSgp)){
    nextSgp=now+1000;
    if(!sgpReady){const uint8_t init[]={0x20,0x03};sgpReady=writeI2c(thBus,0x58,init,2);sgpSince=now;}
    else{
      const uint8_t cmd[]={0x20,0x08};uint8_t result[6];
      if(writeI2c(thBus,0x58,cmd,2)){
        delay(12);
        if(readI2c(thBus,0x58,result,6)&&crc8(result)==result[2]&&crc8(result+3)==result[5]&&uint32_t(now-sgpSince)>=15000)
          readings[4].set((result[3]<<8)|result[4],now);
      }
    }
  }
  digitalWrite(4,indicator&&readings[1].valid(now)&&readings[1].value>1000);
  digitalWrite(33,indicator&&readings[0].valid(now)&&readings[0].value>50);
  digitalWrite(32,indicator&&((hchoPresent&&readings[5].valid(now)&&readings[5].value>0.1f)||(!hchoPresent&&readings[4].valid(now)&&readings[4].value>300)));
  digitalWrite(25,WiFi.status()!=WL_CONNECTED);
}
