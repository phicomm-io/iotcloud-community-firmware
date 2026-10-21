#include "bemfa.h"
#include "json.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int json_heap_permit(size_t n){(void)n;return 1;}
static void values(bemfa_config_t *c,const char *wire,bool valid) {
    json_object *v=json_tokener_parse(wire);assert(v);bool actual=bemfa_config_values(c,v,true);if(actual!=valid)fprintf(stderr,"wire=%s expected=%d got=%d\n",wire,valid,actual);assert(actual==valid);json_object_put(v);
}
int main(void) {
    bemfa_config_t c;bemfa_defaults(&c,"tc1","aabbccddeeff");
    assert(c.mqtt_enabled && !c.enabled && bemfa_config_valid(&c,true));
    for(unsigned i=0;i<6;i++){assert(strstr(c.topics[i],"001"));for(unsigned j=0;j<i;j++)assert(strcmp(c.topics[i],c.topics[j]));}
    values(&c,"{\"enabled\":true}",false);assert(!bemfa_config_valid(&c,true));
    values(&c,"{\"app_id\":\"user-app\",\"secret_key\":\"private-key\"}",true);
    values(&c,"{\"enabled\":false}",true);assert(!strcmp(c.secret_key,"private-key"));
    values(&c,"{\"secret_key\":\"\"}",true);assert(!c.secret_key[0]);
    values(&c,"{\"enabled\":1}",false);
    values(&c,"{\"unknown\":true}",false);
    values(&c,"{\"topics\":[\"same001\",\"same001\",\"\",\"\",\"\",\"\"]}",false);
    values(&c,"{\"topics\":[\"wrong003\",\"\",\"\",\"\",\"\",\"\"]}",false);
    values(&c,"{\"topics\":[\"wild+001\",\"\",\"\",\"\",\"\",\"\"]}",false);
    values(&c,"{\"topics\":[\"ok001\",\"\",\"\",\"\",\"\",\"\"]}",true);
    bemfa_defaults(&c,"a1","112233445566");assert(bemfa_config_valid(&c,false));
    bemfa_command_t command;
    assert(bemfa_parse(BEMFA_SOCKET,"on",2,&command)&&command.on&&!command.has_value);
    assert(bemfa_parse(BEMFA_SOCKET,"off",3,&command)&&!command.on);
    static const unsigned speeds[]={20,50,75,100};
    for(unsigned i=0;i<4;i++){char wire[5];snprintf(wire,sizeof(wire),"on#%u",i+1);assert(bemfa_parse(BEMFA_FAN,wire,4,&command));assert(command.value==speeds[i]&&command.on&&command.has_value);assert(bemfa_fan_gear(speeds[i])==i+1);}
    assert(bemfa_fan_gear(35)==2);assert(bemfa_fan_gear(62)==2);assert(bemfa_fan_gear(63)==3);
    assert(bemfa_fan_gear(1)==1&&bemfa_fan_gear(100)==4);
    const char *bad[]={"on#0","on#5","on#20","on#1#1","ON","off#1","on#-1","on#1 ","on#01","on#9999999999999",NULL};
    for(unsigned i=0;bad[i];i++)assert(!bemfa_parse(BEMFA_FAN,bad[i],strlen(bad[i]),&command));
    assert(!bemfa_parse(BEMFA_SOCKET,"on#1",4,&command));
    assert(bemfa_parse(BEMFA_LIGHT,"on#100",6,&command)&&command.value==100);
    assert(bemfa_parse(BEMFA_LIGHT,"on#1",4,&command)&&command.value==1);
    assert(!bemfa_parse(BEMFA_LIGHT,"on#101",6,&command));assert(!bemfa_parse(BEMFA_LIGHT,"on#50#fff",9,&command));
    assert(!bemfa_parse(BEMFA_LIGHT,"on\0",3,&command));
    puts("Bemfa: defaults, credential omission/clear, validation, six socket topics, strict fan/light parsing and nearest gears PASS");
}
