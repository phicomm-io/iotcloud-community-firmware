#include <ArduinoJson.h>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>
extern "C" {
#include "protocol/display_policy.h"
#include "protocol/scene.h"
#include "protocol/screen_caps.h"
}
static struct { bool extended=true;uint32_t capabilities=SCREEN_CAP_DISPLAY,session=1; } screenBridge;
static struct { bool active=false; } screenImage;
static display_policy screenDisplayPolicy;
static screen_scene screenScene;
static uint32_t screenPolicyAck,screenPolicySentAt,screenPolicySession,ms;
static const char *screenPolicyError="";
static bool save_ok=true;
static unsigned saved;
static std::vector<uint8_t> sent;
static uint32_t millis(){return ms;}
static bool screenSave(){saved++;return save_ok;}
static void invalidateBusinessStatusCache(){}
static bool bry_bridge_send(void *,const void *p,size_t n,unsigned cap){assert(cap==SCREEN_CAP_DISPLAY);sent.assign((const uint8_t*)p,(const uint8_t*)p+n);return true;}
namespace bry { static void textCopy(char *out,size_t n,const char *in){std::strncpy(out,in,n-1);out[n-1]=0;} }
#include "display_policy.inc"
int main(){
 display_policy_default(&screenDisplayPolicy);scene_defaults(&screenScene);
 DynamicJsonDocument request(4096),reply(4096);
 auto params=request.to<JsonObject>();params["ceiling"]=40;params["external_minutes"]=2;params["battery_minutes"]=1;params["idle_brightness"]=8;params["idle_page"]=0;
 auto plans=params.createNestedArray("plans");auto p=plans.createNestedObject();p["name"]="night";p["enabled"]=true;p["days"]=16;p["start"]=1380;p["end"]=420;p["brightness"]=5;p["page"]=1;
 auto call=[&](){reply.clear();return screenPolicySet(params,reply.to<JsonObject>());};
 assert(!strcmp(call(),"accepted")&&screenDisplayPolicy.revision==2&&saved==1);
 ms=10;screenPolicyPoll();display_policy decoded;assert(display_policy_decode(&decoded,sent.data(),sent.size()));assert(decoded.plans[0].days==16&&decoded.idle_brightness==8);
 screenPolicyAck=2;sent.clear();ms=5000;screenPolicyPoll();assert(sent.empty());
 screenBridge.session++;screenPolicyPoll();assert(!sent.empty());
 p["days"]=0;assert(!strcmp(call(),"invalid_params")&&screenDisplayPolicy.revision==2);p["days"]=16;
 p["start"]=420;assert(!strcmp(call(),"invalid_params"));p["start"]=1380;
 params["ceiling"]=-1;assert(!strcmp(call(),"invalid_params"));params["ceiling"]=40;
 p["page"]=7;assert(!strcmp(call(),"invalid_params"));p["page"]=1;
 save_ok=false;assert(!strcmp(call(),"storage_error")&&screenDisplayPolicy.revision==2);save_ok=true;
 screenImage.active=true;assert(!strcmp(call(),"busy"));screenImage.active=false;
 auto old=screenScene;std::swap(screenScene.page[0],screenScene.page[1]);screenPolicyRemap(old,screenScene);
 assert(screenDisplayPolicy.idle_page==1&&screenDisplayPolicy.plans[0].page==0);
 old=screenScene;screenScene.page[0].enabled=false;screenPolicyRemap(old,screenScene);assert(screenDisplayPolicy.plans[0].page==255);
 screenBridge.capabilities=0;assert(!strcmp(call(),"unsupported"));
 return 0;
}
