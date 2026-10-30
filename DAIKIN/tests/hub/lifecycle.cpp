/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "hub_lan_client.h"
#include <atomic>
#include <memory>
#include <vector>
#include <deque>
#include <algorithm>
#include <cassert>
#include <cstdio>
using String=std::string;using esp_mqtt_client_handle_t=void*;using QueueHandle_t=void*;
#include "globals.inc"
static std::deque<HubMessage> queue;
static std::vector<std::string> events;
static bool mqttRestart=false,wifiRestart=false,configRecovery=false;
static String hubHttpToken="owner-session";
static iotcloud::HubLanClient client;
static unsigned sent=0,completed=0;
static void hubLanReset(){events.push_back("udp-reset");client.reset();}
static void homeQuiesce(){events.push_back("home-quiesce");}
static void homeChanged(){events.push_back("home-changed");}
struct Service{bool ready(){return true;}void clearSessions(){events.push_back("sessions-clear");}} hubService;
struct Bus{void(*dispatch)()=nullptr;} hubDeviceBus;
struct Relay{void reset(){events.push_back("relay-reset");}} hubRelay;
static bool hubAllocationReady(size_t){return true;}
static const int pdTRUE=1;
static unsigned pdMS_TO_TICKS(unsigned ms){return ms;}
static bool consumeOnWait=false;
static unsigned waitedSends=0;
static int xQueueSend(QueueHandle_t,const HubMessage*p,int wait){
 assert(wait==50);
 if(queue.size()==3&&consumeOnWait){queue.pop_front();++waitedSends;}
 if(queue.size()==3)return 0;
 queue.push_back(*p);return pdTRUE;
}
static void xQueueReset(QueueHandle_t){events.push_back("inbox-reset");queue.clear();}
struct Event {int current_data_offset=0,topic_len=0,total_data_len=0,data_len=0;const char *topic=nullptr,*data=nullptr;bool retain=false;};
using esp_mqtt_event_handle_t=Event*;
#include "capture.inc"
static void stopMqtt(){events.push_back("mqtt-join");}
static void stopBemfa(){events.push_back("bemfa-join");bemfaConnected=false;bemfaTopics.clear();}
static void startBemfa(){events.push_back("bemfa-start");}
#include "lifecycle.inc"
#include "reload.inc"
static void noop(){}
static void begin(){uint8_t token[16]={};assert(client.begin("02c400000001",7,"02c400000002","10.0.0.2",11288,token,"route",std::string(1,char(0x80)),1));assert(client.busy());
 // Hold an owned, partially received 1000-byte response before lifecycle reset.
 uint8_t chunk[LAN_V4_CHUNK]={};lan_v4_packet part={};part.op=LAN_V4_RESULT;strcpy(part.src,"02c400000002");strcpy(part.dst,"02c400000001");strcpy(part.code,"ok");part.id=client.lastId;part.request_boot=7;part.boot=9;part.total=2;part.size=1000;part.body=chunk;part.body_length=sizeof(chunk);assert(client.receive(part,"10.0.0.2",11288,2));assert(client.busy());}
static void seedInbox(){
 hubInbox=reinterpret_cast<void*>(1);hubRemoteTopics={"up"};bemfaTopics={"bemfa"};
 for(unsigned source=0;source<2;source++){
  const char *topic=source?"bemfa":"up";Event event;event.topic=topic;event.topic_len=strlen(topic);event.total_data_len=1;event.data="x";event.data_len=1;assert(hubCapture(&event,source));
  event.total_data_len=2;assert(hubCapture(&event,source));assert(hubMessages[source]&&hubMessages[source]->length==1);
 }
 assert(queue.size()==2);
}
static size_t index(const char *event){auto i=std::find(events.begin(),events.end(),event);assert(i!=events.end());return size_t(i-events.begin());}
int main(){
 // Model loop() freeing a bounded inbox slot while the SDK callback yields.
 hubInbox=reinterpret_cast<void*>(1);hubRemoteTopics={"up"};
 Event burst;burst.topic="up";burst.topic_len=2;burst.total_data_len=burst.data_len=1;burst.data="x";
 for(unsigned i=0;i<3;++i)assert(hubCapture(&burst,0));
 assert(queue.size()==3&&!hubInboxDropped);
 consumeOnWait=true;burst.data="y";assert(hubCapture(&burst,0));consumeOnWait=false;
 assert(waitedSends==1&&queue.size()==3&&!hubInboxDropped&&queue.back().body[0]=='y'&&!hubMessages[0]);
 assert(hubCapture(&burst,0));assert(queue.size()==3&&hubInboxDropped==1&&!hubMessages[0]);
 queue.clear();hubInboxDropped=0;

 (void)bemfaMqtt;hubUpstreamChanged();assert(hubReload&&!mqttRestart);events.clear();
 client.authorize=[](const std::string&,const std::string&,uint16_t,const uint8_t*){return true;};
 client.send=[](const std::string&,uint16_t,const void*,size_t){++sent;return true;};
 client.complete=[](const std::string&,const void*,size_t){++completed;};
 seedInbox();begin();uint32_t first=client.lastId,epoch=hubEpoch;hubReload=true;hubCredentials=true;pollHub(2);
 assert(!client.busy()&&client.lastId==first&&hubEpoch==epoch+1&&queue.empty()&&!hubMessages[0]&&!hubMessages[1]);
 assert(index("mqtt-join")<index("inbox-reset")&&index("bemfa-join")<index("inbox-reset")&&index("inbox-reset")<index("bemfa-start"));
 for(unsigned source=0;source<2;source++){Event tail;tail.current_data_offset=1;tail.total_data_len=2;tail.data_len=1;tail.data="y";assert(!hubCapture(&tail,source));}assert(queue.empty());
 unsigned sends=sent;client.tick(1000);assert(sent==sends&&completed==0);assert(mqttRestart&&!hubReload&&!hubCredentials);
 // A completion from the retired transaction cannot be delivered after reset.
 lan_v4_packet packet={};packet.op=LAN_V4_RESULT;strcpy(packet.src,"02c400000002");strcpy(packet.dst,"02c400000001");packet.id=first;packet.request_boot=7;assert(!client.receive(packet,"10.0.0.2",11288,1001));
 events.clear();seedInbox();begin();assert(client.lastId>first);epoch=hubEpoch;hubDeviceBus.dispatch=noop;hubReload=hubBirth=hubCredentials=wifiRestart=true;hubRestoreChanged();
 assert(hubRestoring&&!client.busy()&&client.lastId>first&&hubEpoch==epoch+1&&queue.empty()&&!hubMessages[0]&&!hubMessages[1]);
 assert(!hubDeviceBus.dispatch&&hubHttpToken.empty()&&hubRemoteTopics.empty()&&!hubReload&&!hubBirth&&!hubCredentials&&!wifiRestart&&!mqttRestart);
 assert(index("home-quiesce")<index("udp-reset")&&index("sessions-clear")<index("udp-reset")&&index("mqtt-join")<index("inbox-reset")&&index("bemfa-join")<index("inbox-reset"));
 Event event;event.topic="up";event.topic_len=2;event.total_data_len=event.data_len=1;event.data="x";assert(!hubCapture(&event,0));assert(!hubCapture(&event,1));sends=sent;client.tick(2000);pollHub(2000);assert(sent==sends&&queue.empty()&&completed==0);
 puts("production UDP lifecycle PASS: restore/reload cancellation, monotonic IDs, stale results, joined source generations/inbox retirement");
}
