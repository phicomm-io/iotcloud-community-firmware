#include "weather_retry.h"
#include "layout_upload.h"
#include <ArduinoJson.h>
#include <cassert>
#include <cstring>
#include <string>
int main(){
  WeatherRetry r;
  assert(!r.memoryReady(200000,16000,true));
  assert(!r.memoryReady(73000,100000,true));
  assert(r.memoryReady(73728,24576,true));
  assert(!r.memoryReady(73728,24576,false));
  assert(r.due(0,false));r.begin(0);assert(!r.due(14999,true));
  r.failed(15000);assert(r.remaining(15000)==30000&&!r.due(29999,true));
  for(auto seconds:{60u,120u,240u,300u,300u}){r.failed(15000);assert(r.remaining(15000)==seconds*1000);}
  r.success(UINT32_MAX-1000,900);assert(!r.failures&&r.remaining(1000)==897999);
  assert(!r.due(1000,false));assert(r.due(900000,false));
  // Exercise the real scene codec: commit remains atomic, failed/abandoned
  // requests relinquish RAM, and a later valid upload still works.
  ScreenLayoutUpload upload;screen_scene scene;scene_defaults(&scene);
  uint8_t begin[13]={SC_MSG_BEGIN},commit[5]={SC_MSG_COMMIT};
  uint32_t revision=scene.revision;scene_put32(begin+1,revision+1);
  scene_put32(begin+5,100);assert(!upload.receive(&scene,begin,13,0)&&!upload.active());
  scene_put32(begin+5,SCENE_BYTES);assert(upload.receive(&scene,begin,13,UINT32_MAX-10000));
  upload.expire(19998);assert(upload.active());upload.expire(19999);assert(!upload.active());
  assert(scene.revision==revision);
  screen_scene next=scene;next.revision++;uint8_t bytes[SCENE_BYTES];assert(scene_encode(&next,bytes));
  scene_put32(begin+9,scene_crc32(bytes,sizeof(bytes)));assert(upload.receive(&scene,begin,13,30000));
  uint8_t chunk[517]={SC_MSG_CHUNK};
  for(unsigned at=0;at<sizeof(bytes);){unsigned n=sizeof(bytes)-at;if(n>512)n=512;scene_put32(chunk+1,at);memcpy(chunk+5,bytes+at,n);assert(upload.receive(&scene,chunk,n+5,30000+at));at+=n;}
  scene_put32(commit+1,next.revision);assert(upload.receive(&scene,commit,5,40000));
  assert(!upload.active()&&scene.revision==next.revision);
  assert(upload.receive(&scene,begin,13,50000));scene_put32(chunk+1,1);
  assert(!upload.receive(&scene,chunk,6,50000)&&!upload.active());
  assert(scene.revision==next.revision);
  // Longest allowed credentials, host and UTF-8 label fit the reduced pool,
  // including a const-input deserialize and an independent owned copy.
  DynamicJsonDocument config(1536),copy(1536);
  config["enabled"]=true;config["verify_tls"]=false;config["provider"]="qweather";
  config["host"]=std::string(127,'a');config["key"]=std::string(256,'k');
  config["label"]=std::string(48,'l');config["latitude"]=90.0f;config["longitude"]=180.0f;config["interval"]=86400;
  std::string wire;serializeMsgPack(config,wire);assert(!config.overflowed());
  assert(!deserializeMsgPack(copy,wire)&&!copy.overflowed());
  config.clear();assert(copy["key"].as<std::string>().size()==256);
}
