#include "../common/mqtt_account.h"
#include <assert.h>
int main(void) {
    mqtt_account_t a,b; char topic[MQTT_TOPIC_MAX], id[MQTT_TOPIC_MAX];
    assert(mqtt_account_init(&a,1,"m1","Alice@example.com","AABBCCDDEEFF"));
    assert(!strcmp(a.key,"u_416c696365406578616d706c652e636f6d"));
    assert(!strcmp(a.mac,"aabbccddeeff"));
    assert(mqtt_leaf(&a,"brightness/set",topic,sizeof(topic)));
    assert(!strcmp(topic,"phicomm/m1/u_416c696365406578616d706c652e636f6d/aabbccddeeff/brightness/set"));
    assert(mqtt_account_init(&b,1,"m1","alice@example.com","AABBCCDDEEFF"));
    assert(strcmp(a.key,b.key));
    assert(mqtt_account_init(&b,1,"a1"," 用户 ","aabbccddeeff"));
    assert(!strcmp(b.key,"u_20e794a8e688b720"));
    assert(mqtt_ha(&b,"fan","fan",topic,sizeof(topic)));
    assert(!strcmp(topic,"homeassistant/fan/u_20e794a8e688b720/a1_aabbccddeeff_fan/config"));
    assert(mqtt_identity(&b,"fan",id,sizeof(id)));
    assert(!strcmp(id,"u_20e794a8e688b720_a1_aabbccddeeff_fan"));
    assert(mqtt_aux(&b,"bind","112233445566",topic,sizeof(topic)));
    assert(!strcmp(topic,"phicomm/bind/u_20e794a8e688b720/112233445566"));
    char user[65]; memset(user,'x',63); user[63]=0;
    assert(mqtt_account_init(&b,1,"tc1",user,"aabbccddeeff"));
    assert(strlen(b.key)==128);
    assert(mqtt_ha(&b,"sensor","powerConsumptionYesterday",topic,sizeof(topic)));
    user[63]='x';user[64]=0; assert(!mqtt_account_init(&b,1,"tc1",user,"aabbccddeeff"));
    assert(!mqtt_account_init(&b,1,"tc1","","aabbccddeeff"));
    assert(!mqtt_account_init(&b,1,"tc1","alice","not-a-mac"));
    assert(!mqtt_hex(id,0,"x"));
    assert(!mqtt_format(id,2,"%s","too long") && !*id);
    assert(mqtt_account_init(&b,0,"m1","","aabbccddeeff"));
    assert(!strcmp(b.base,"phicomm/m1/aabbccddeeff"));
    assert(mqtt_ha(&b,"number","brightness",topic,sizeof(topic)));
    assert(!strcmp(topic,"homeassistant/number/m1_aabbccddeeff/brightness/config"));
    assert(mqtt_account_init(&b,0,"tc1","old","aabbccddeeff"));
    assert(mqtt_ha(&b,"switch","socket_0",topic,sizeof(topic)));
    assert(!strcmp(topic,"homeassistant/switch/AABBCCDDEEFF/socket_0/config"));
    assert(mqtt_retry_seconds(0)==1 && mqtt_retry_seconds(4)==15 && mqtt_retry_seconds(99)==30);
    puts("MQTT UTF-8 identity, byte boundaries and legacy/account namespace passed");
}
