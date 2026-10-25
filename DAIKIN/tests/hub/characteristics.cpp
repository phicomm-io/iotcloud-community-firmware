/* SPDX-License-Identifier: GPL-3.0-or-later */
// The two methods under test are extracted verbatim from HomeSpan.cpp. Only
// their SDK/service dependencies are replaced; parsing and PASS 1/2 are real.
#include "IoTCloudValues.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <vector>
using boolean=bool;
#include "Characteristics.h"
#include "HAPConstants.h"
#define LOG0(...) ((void)0)
#define LOG1(...) ((void)0)
static unsigned long millis(){return 100;}
static int nvs_set_u64(unsigned,const char *,uint64_t){assert(false);return 0;}
static int nvs_set_str(unsigned,const char *,const char *){assert(false);return 0;}
static int nvs_commit(unsigned){assert(false);return 0;}
static char *stripBackslash(char *text){return text;}
struct HAPClient {};
struct SpanService {
  unsigned calls=0;
  std::function<bool()> action;
  bool update(){++calls;return !action || action();}
};
struct SpanCharacteristic {
  union UVal {
    boolean BOOL;uint8_t UINT8;uint16_t UINT16;uint32_t UINT32;
    uint64_t UINT64;int32_t INT;double FLOAT;char *STRING=nullptr;
  };
  struct EVLIST : std::vector<HAPClient *> {
    void add(HAPClient *client){if(std::find(begin(),end(),client)==end())push_back(client);}
    void remove(HAPClient *client){erase(std::remove(begin(),end(),client),end());}
  } evList;
  uint32_t aid=1,iid;
  FORMAT format;
  uint8_t perms=PW|EV;
  UVal value,newValue,minValue,maxValue;
  SpanService *service;
  char *nvsKey=nullptr,*validValues=nullptr;
  uint8_t updateFlag=0;
  unsigned long updateTime=0;
  SpanCharacteristic(FORMAT type,SpanService &owner,uint32_t id):iid(id),format(type),service(&owner){}
  ~SpanCharacteristic(){if(format>=STRING){free(value.STRING);free(newValue.STRING);}}
  template<class T> void uvSet(UVal &dest,T number){
    switch(format){
      case BOOL:dest.BOOL=number;break;
      case UINT8:dest.UINT8=number;break;
      case UINT16:dest.UINT16=number;break;
      case UINT32:dest.UINT32=number;break;
      case UINT64:dest.UINT64=number;break;
      case INT:dest.INT=number;break;
      case FLOAT:dest.FLOAT=number;break;
      default:assert(false);
    }
  }
  template<class T> T uvGet(UVal &src){
    switch(format){
      case BOOL:return T(src.BOOL);
      case UINT8:return T(src.UINT8);
      case UINT16:return T(src.UINT16);
      case UINT32:return T(src.UINT32);
      case UINT64:return T(src.UINT64);
      case INT:return T(src.INT);
      case FLOAT:return T(src.FLOAT);
      default:assert(false);return 0;
    }
  }
  void uvSet(UVal &dest,const char *text){
    dest.STRING=static_cast<char *>(realloc(dest.STRING,strlen(text)+1));
    assert(dest.STRING);strcpy(dest.STRING,text);
  }
  void uvSet(UVal &dest,UVal &src){
    if(format<STRING)dest=src;
    else uvSet(dest,static_cast<const char *>(src.STRING));
  }
  template<class T> void init(T initial,T minimum,T maximum){
    uvSet(value,initial);uvSet(newValue,initial);uvSet(minValue,minimum);uvSet(maxValue,maximum);
  }
  StatusCode loadUpdate(char *,char *,boolean);
};
struct SpanBuf {
  uint32_t aid=0,iid=0;boolean wr=false;char *val=nullptr,*ev=nullptr;
  StatusCode status;SpanCharacteristic *characteristic=nullptr;
};
struct Span {
  unsigned long snapTime=0;
  unsigned charNVS=0;
  HAPClient *currentClient=nullptr;
  std::map<uint64_t,unsigned long> TimedWrites;
  std::vector<SpanCharacteristic *> characteristics;
  SpanCharacteristic *find(uint32_t aid,uint32_t iid){
    for(auto c:characteristics)if(c->aid==aid && c->iid==iid)return c;
    return nullptr;
  }
  int updateCharacteristics(char *,SpanBuf *,int);
} homeSpan;
#include "characteristic-methods.inc"

static StatusCode load(SpanCharacteristic &c,std::string value,const char *ev=nullptr,bool wr=false){
  std::vector<char> text(value.begin(),value.end());text.push_back(0);
  std::string event=ev?ev:"";
  return c.loadUpdate(text.data(),ev?&event[0]:nullptr,wr);
}
static int request(const std::string &rows,std::vector<SpanBuf> &result){
  const std::string json="{\"characteristics\":["+rows+"]}";
  std::vector<char> text(json.begin(),json.end());text.push_back(0);
  return homeSpan.updateCharacteristics(text.data(),result.data(),result.size());
}
static std::string row(unsigned iid,const std::string &value){
  return "{\"aid\":1,\"iid\":"+std::to_string(iid)+",\"value\":"+value+"}";
}

int main(){
  using namespace iotcloud;
  uint64_t u=17;
  assert(homeParseUnsigned("18446744073709551615",0,UINT64_MAX,u) && u==UINT64_MAX);
  for(auto text:{"18446744073709551616","-1","+1","01","0x1","1.0","1e0","1junk","true","false","null","\"1\"",""," "}){
    u=17;assert(!homeParseUnsigned(text,0,UINT64_MAX,u) && u==17);
  }
  for(unsigned value=0;value<1000;++value){
    const auto text=std::to_string(value);
    assert(homeParseUnsigned(text.c_str(),0,255,u)==(value<=255));
  }
  int32_t s=17;
  assert(homeParseInt("-2147483648",INT32_MIN,INT32_MAX,s) && s==INT32_MIN);
  assert(homeParseInt("2147483647",INT32_MIN,INT32_MAX,s) && s==INT32_MAX);
  for(auto text:{"2147483648","-2147483649","4294967296","-","+0","--1","-01","1.1","1e1","true","1junk"}){
    s=17;assert(!homeParseInt(text,INT32_MIN,INT32_MAX,s) && s==17);
  }
  double f=17;
  for(auto text:{"0","-0","0.0","1.0e2"," 1E+2 \r\n",".5","1.","NaN","inf","-Inf","0x1p1","1e309","1e-9999","1foo","01","1e","1e+","true"}){
    const bool valid=std::string(text)=="0" || std::string(text)=="-0" || std::string(text)=="0.0" ||
      std::string(text)=="1.0e2" || std::string(text)==" 1E+2 \r\n";
    f=17;assert(homeParseFloat(text,0,100,f)==valid);if(!valid)assert(f==17);
  }
  assert(homeValueInList("[-1,0,3]",-1) && homeValueInList("[-1,0,3]",3));
  assert(!homeValueInList("[-1,0,3]",1) && !homeValueInList("[1,]",1));

  HAPClient client;homeSpan.currentClient=&client;
  SpanService fan;
  SpanCharacteristic active(UINT8,fan,10),speed(FLOAT,fan,11),readOnly(INT,fan,12);
  active.init<uint8_t>(1,0,1);speed.init<double>(25,0,100);readOnly.init<int32_t>(7,0,100);
  readOnly.perms=EV;
  unsigned commands=0;bool activeWritten=false,speedWritten=false,failService=false;
  fan.action=[&](){
    activeWritten=active.updateFlag;speedWritten=speed.updateFlag;
    if(activeWritten || speedWritten)++commands;
    return !failService;
  };
  homeSpan.characteristics={&active,&speed,&readOnly};
  std::vector<SpanBuf> rows(2);
  for(bool invalidFirst:{false,true}){
    auto valid=row(11,"50"),invalid=row(10,"256");
    assert(request(invalidFirst?invalid+","+valid:valid+","+invalid,rows));
    assert(rows[invalidFirst?0:1].status==StatusCode::InvalidValue);
    assert(rows[invalidFirst?1:0].status==StatusCode::OK);
    assert(!activeWritten && speedWritten && active.value.UINT8==1 && active.newValue.UINT8==1);
    assert(active.updateFlag==0 && speed.updateFlag==0 && active.updateTime==0);
    rows=std::vector<SpanBuf>(2);
  }
  for(auto invalid:{"-1","2","256","4294967296","1junk","1.0","1e0","true","\"0\""}){
    const unsigned before=commands,calls=fan.calls;
    rows=std::vector<SpanBuf>(1);
    assert(request(row(10,invalid),rows) && rows[0].status==StatusCode::InvalidValue);
    assert(commands==before && fan.calls==calls && active.value.UINT8==1 && active.newValue.UINT8==1);
  }
  rows=std::vector<SpanBuf>(2);
  assert(request(row(11,"40")+","+row(12,"0"),rows));
  assert(rows[0].status==StatusCode::OK && rows[1].status==StatusCode::ReadOnly && readOnly.value.INT==7);
  assert(!readOnly.updateFlag);
  const auto oldActive=active.newValue.UINT8;
  assert(load(active,"256","true")==StatusCode::InvalidValue && active.evList.empty());
  assert(active.newValue.UINT8==oldActive && !active.updateFlag);
  // An event-only row has no pending write and must survive another write to
  // the same service without copying stale newValue into its current value.
  active.newValue.UINT8=0;
  rows=std::vector<SpanBuf>(2);
  assert(request(row(11,"30")+",{\"aid\":1,\"iid\":10,\"ev\":true}",rows));
  assert(rows[0].status==StatusCode::OK && rows[1].status==StatusCode::OK);
  assert(active.value.UINT8==1 && active.evList.size()==1 && !active.updateFlag);
  active.newValue=active.value;
  const unsigned before=commands;
  rows=std::vector<SpanBuf>(2);
  assert(!request(row(10,"0")+","+row(10,"1"),rows));
  assert(commands==before && active.value.UINT8==1 && !active.updateFlag);
  for(const auto &bad:{
      "{\"aid\":4294967297,\"iid\":10,\"value\":0}",
      "{\"aid\":1junk,\"iid\":10,\"value\":0}",
      "{\"aid\":1,\"iid\":10,\"iid\":11,\"value\":0}",
      "{\"aid\":1,\"iid\":10,\"value\":0,\"value\":1}",
      "{\"aid\":1,\"iid\":10,\"value\":0,\"r\":junk}",
      "{\"aid\":1,\"iid\":10,\"value\":0,\"pid\":18446744073709551616}"}){
    rows=std::vector<SpanBuf>(1);assert(!request(bad,rows));
    assert(commands==before && active.value.UINT8==1 && !active.updateFlag);
  }
  // Service rejection rolls validated values back and clears all flags.
  failService=true;rows=std::vector<SpanBuf>(2);
  assert(request(row(10,"0")+","+row(11,"99"),rows));
  assert(rows[0].status==StatusCode::Unable && rows[1].status==StatusCode::Unable);
  assert(active.value.UINT8==1 && active.newValue.UINT8==1 && speed.value.FLOAT==30 && speed.newValue.FLOAT==30);
  assert(!active.updateFlag && !speed.updateFlag);

  SpanCharacteristic integer(INT,fan,20);integer.init<int32_t>(0,INT32_MIN,INT32_MAX);
  assert(load(integer,"-2147483648")==StatusCode::TBD && integer.newValue.INT==INT32_MIN);
  integer.updateFlag=0;
  assert(load(integer,"2147483648")==StatusCode::InvalidValue && !integer.updateFlag && integer.newValue.INT==INT32_MIN);
  integer.validValues=const_cast<char *>("[-1,0,3]");
  assert(load(integer,"2")==StatusCode::InvalidValue);
  assert(load(integer,"-1")==StatusCode::TBD && integer.newValue.INT==-1);
  for(auto type:{UINT8,UINT16,UINT32,UINT64}){
    SpanCharacteristic counter(type,fan,21);
    const uint64_t maximum=type==UINT8?UINT8_MAX:type==UINT16?UINT16_MAX:type==UINT32?UINT32_MAX:UINT64_MAX;
    counter.init<uint64_t>(0,0,maximum);
    assert(load(counter,std::to_string(maximum))==StatusCode::TBD && counter.uvGet<uint64_t>(counter.newValue)==maximum);
    counter.updateFlag=0;
    assert(load(counter,type==UINT64?"18446744073709551616":std::to_string(maximum+1))==StatusCode::InvalidValue);
    assert(!counter.updateFlag && counter.uvGet<uint64_t>(counter.newValue)==maximum);
  }
  SpanCharacteristic flag(BOOL,fan,22);flag.init<bool>(true,false,true);
  for(auto good:{"true","false","0","1"," \ttrue\r\n"})assert(load(flag,good)==StatusCode::TBD);
  flag.updateFlag=0;
  for(auto bad:{"2","-1","1.0","truejunk","\"true\""})assert(load(flag,bad)==StatusCode::InvalidValue && !flag.updateFlag);
  for(auto bad:{"-0.1","100.1","NaN","Inf","1e309","1junk","\"50\""}){
    const auto old=speed.newValue.FLOAT;
    assert(load(speed,bad)==StatusCode::InvalidValue && speed.newValue.FLOAT==old && !speed.updateFlag);
  }
  SpanService textService;
  SpanCharacteristic label(STRING,textService,30);
  homeSpan.characteristics.push_back(&label);
  rows=std::vector<SpanBuf>(1);
  assert(request(row(30," \"new name\" "),rows) && rows[0].status==StatusCode::OK);
  assert(!strcmp(label.value.STRING,"new name") && !label.updateFlag);
  assert(load(label,"unquoted")==StatusCode::InvalidValue && !strcmp(label.value.STRING,"new name"));
  puts("HAP characteristic validation and service dispatch passed");
}
