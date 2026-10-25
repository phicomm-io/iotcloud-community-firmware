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
    co2Module.push(co2Uart.read(),now);
  }
  uint8_t command[6];size_t commandSize=co2Module.poll(now,command);
  if(commandSize){while(co2Uart.available())co2Uart.read();co2Uart.write(command,commandSize);}
  if(co2Module.valid(now))readings[CO2].set(co2Module.raw,co2Module.measuredAt);
  else readings[CO2].received=false;
  while(hchoUart.available()){
    uint8_t b=hchoUart.read();if(!hchoWaiting)continue;
    if(hchoUsed==0&&b!=0xff)continue;
    hchoFrame[hchoUsed++]=b;
    if(hchoUsed==9){uint16_t v;if(daikin::ch2o(hchoFrame,9,v)){hchoPresent=true;if(warmedUp)readings[5].set(v/1000.0f,now);}hchoUsed=0;hchoWaiting=false;}
  }
  if(hchoWaiting&&uint32_t(now-hchoSent)>500){hchoUsed=0;hchoWaiting=false;}
  if(due(now,nextSample)){
    nextSample=now+5000;
    uint8_t p[12];daikin::Particulates particles;
    if(readI2c(pmBus,0x28,p,12)){
      bool decoded=daikin::pm(p,12,false,particles);
      if(decoded)pmModel="PM2105";
      else if((decoded=daikin::pm(p,12,true,particles)))pmModel="PM2005";
      if(decoded){
        if(particles.pm25<=10000)readings[PM25].set(particles.pm25,now);
        if(particles.pm10<=10000)readings[PM10].set(particles.pm10,now);
      }
    }
    // HDC1080 temperature+humidity acquisition; bounded 15 ms conversion.
    const uint8_t reg[]={0};
    if(writeI2c(thBus,0x40,reg,1)){
      delay(15);uint8_t th[4];
      if(readI2c(thBus,0x40,th,4)){
        float t=((th[0]<<8)|th[1])*165.0f/65536.0f-40;
        float h=((th[2]<<8)|th[3])*100.0f/65536.0f;
        float compensatedT,compensatedH;
        if(daikin::compensateClimate(t,h,compensatedT,compensatedH,config["climate"]["temperature_offset"]|4.9f,config["climate"]["humidity_offset"]|0.0f)){readings[2].set(compensatedT,now);readings[3].set(compensatedH,now);}
      }
    }
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
}
