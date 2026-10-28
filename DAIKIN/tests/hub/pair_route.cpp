#define main cache_fixture_main
#include "status_cache.cpp"
#undef main
static bool localAuthReady=true,storageOkay=true;
static const size_t configCapacity=16384;
static const unsigned LC_PAIR=1,LC_RESUME=2,LC_READY=3,LC_FAILED=4,LC_UNPAIR=5,LC_SLOTS=12;
struct lc_job{unsigned phase=LC_READY;};
struct {}lanClient,lanOps;
static lc_job job;
static unsigned saves=0,starts=0;
static std::string startedIP;
static void initLanClient(){}
struct IPAddress{bool fromString(const char *ip){return ip&&(!strcmp(ip,"192.0.2.10")||!strcmp(ip,"192.0.2.20"));}};
static bool lc_peer(const char *peer){return peer&&strlen(peer)==12;}
static lc_job *lc_find(decltype(lanClient)*,const char *,bool){return &job;}
static bool saveConfig(DynamicJsonDocument &candidate){
 ++saves;if(!storageOkay)return false;return config.set(candidate);
}
static bool lc_start(decltype(lanClient)*,decltype(lanOps)*,const char *peer,const char *ip,const char *password,unsigned scopes,const char **code){
 // The persistence hook must have succeeded before the pairing machine starts.
 assert(config["lan_peers"][0]["peer"]==peer&&config["lan_peers"][0]["ip"]==ip);
 assert(!strcmp(password,"host-test-password")&&scopes==1);
 ++starts;startedIP=ip;*code="accepted";return true;
}
#include "pair-route.inc"
int main(){
 auto peers=config.createNestedArray("lan_peers");auto peer=peers.createNestedObject();
 peer["peer"]="001122334455";peer["ip"]="192.0.2.10";
 const char *code=nullptr;
 assert(lanPairStart("001122334455","192.0.2.20","host-test-password",1,&code));
 assert(saves==1&&starts==1&&startedIP=="192.0.2.20"&&!strcmp(code,"accepted"));
 assert(config["lan_peers"].size()==1);std::string text;serializeJson(config,text);
 assert(text.find("host-test-password")==std::string::npos&&text.find("password")==std::string::npos);
 storageOkay=false;
 assert(!lanPairStart("001122334455","192.0.2.10","host-test-password",1,&code));
 assert(saves==2&&starts==1&&!strcmp(code,"storage_error")&&config["lan_peers"][0]["ip"]=="192.0.2.20");
 storageOkay=true;
 for(unsigned phase:{LC_PAIR,LC_RESUME,LC_UNPAIR}){
 job.phase=phase;
 assert(!lanPairStart("001122334455","192.0.2.10","host-test-password",1,&code));
 assert(saves==2&&starts==1&&!strcmp(code,"busy")&&config["lan_peers"][0]["ip"]=="192.0.2.20");
 }
}
