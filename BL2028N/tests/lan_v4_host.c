/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Real UDP sockets, shared authentication/v4, and actual S30 typed business.
 * Synthetic identity and stdin fault controls are restricted to this host. */
#define _POSIX_C_SOURCE 200809L
#include "../src/ph_lan_v4.h"
#include "../src/device.h"
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
static ph_device device;
static ph_auth_state auth;
static ph_auth_store saved_auth;
static ph_lan_v4 endpoint;
static uint32_t advance_ms;
static size_t available=32768;
static unsigned relay_writes,dispatches,auth_saves;
static bool fail_reply,alias_next;
static char last_receipt_key[65];
static uint32_t clock_ms(void *unused)
{(void)unused;struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint32_t)(t.tv_sec*1000u+t.tv_nsec/1000000u)+advance_ms;}
static bool random_bytes(void *unused,uint8_t *out,size_t length)
{(void)unused;return getrandom(out,length,0)==(ssize_t)length;}
static bool password(void *unused,char out[33])
{(void)unused;strcpy(out,"test-pass");return true;}
static bool save_auth(void *unused,const ph_auth_store *store)
{(void)unused;saved_auth=*store;auth_saves++;return true;}
static bool save_device(void *unused,const ph_settings *settings)
{(void)unused;(void)settings;return true;}
static void relay(void *unused,bool on)
{(void)unused;(void)on;relay_writes++;}
static size_t free_heap(void *unused)
{(void)unused;return available;}
static bool digest(void *unused,const void *bytes,size_t length,uint8_t out[32])
{(void)unused;return SHA256(bytes,length,out)!=NULL;}
static size_t dispatch(void *unused,const ph_lan_v4_request *request,const ph_auth_context *context,
                       void *output,size_t capacity,bool *admitted)
{
    (void)unused;assert(ph_auth_context_valid(context,PH_AUTH_SCOPE_CONTROL)||ph_auth_context_valid(context,PH_AUTH_SCOPE_SENSOR));dispatches++;
    strcpy(last_receipt_key,request->receipt_key);
    size_t n=ph_command_context(&device,request->action,&request->params,request->id,
        request->receipt_key,context,PH_WIRE_MSGPACK,output,capacity,admitted);
    if(fail_reply){fail_reply=false;return 0;}return n;
}
/* Exercise the actual HTTP/MQTT business domain without a second transport
 * implementation. Wire-level HTTP/MQTT ownership has its own socket suites. */
static void remote_command(const char *text)
{
    unsigned char bytes[PH_INPUT_MAX],output[PH_OUTPUT_MAX];size_t n=0;
    while(*text&&*text!='\n'){
        unsigned value=0;
        if(n==sizeof(bytes)||!text[1]||sscanf(text,"%2x",&value)!=1){puts("REMOTE INVALID");return;}
        for(unsigned i=0;i<2;i++)if(!((text[i]>='0'&&text[i]<='9')||(text[i]>='a'&&text[i]<='f'))){puts("REMOTE INVALID");return;}
        bytes[n++]=(unsigned char)value;text+=2;
    }
    size_t k=ph_command(&device,bytes,n,PH_WIRE_MSGPACK,PH_WIRE_MSGPACK,true,false,output,sizeof(output));
    fputs("REMOTE ",stdout);for(size_t i=0;i<k;i++)printf("%02x",output[i]);putchar('\n');
}
static void console(const char *line)
{
    unsigned value;
    if(sscanf(line,"HEAP %u",&value)==1){available=value;printf("HEAP %zu\n",available);}
    else if(sscanf(line,"ADVANCE %u",&value)==1){advance_ms+=value;ph_lan_v4_poll(&endpoint);printf("ADVANCE %u\n",advance_ms);}
    else if(!strncmp(line,"FAIL_REPLY",10)){fail_reply=true;puts("FAIL_REPLY OK");}
    else if(!strncmp(line,"ALIAS",5)){alias_next=true;puts("ALIAS OK");}
    else if(!strncmp(line,"PAUSE",5)){ph_lan_v4_pause(&endpoint);puts("PAUSE OK");}
    else if(!strncmp(line,"REVOKE",6)){assert(ph_auth_password_changed(&auth));ph_lan_v4_revoke(&endpoint);puts("REVOKE OK");}
    else if(!strncmp(line,"KEY",3))printf("KEY %s\n",last_receipt_key);
    else if(!strncmp(line,"REMOTE ",7))remote_command(line+7);
    else if(!strncmp(line,"SENSOR_GRANT ",13)){
        char peer[13],token[33];unsigned boot;
        bool ok=sscanf(line+13,"%12s %32s %u",peer,token,&boot)==3&&ph_lan_v4_sensor_grant(&endpoint,peer,token,boot);
        printf("SENSOR_GRANT %s\n",ok?"OK":"DENIED");
    }
    else if(!strncmp(line,"MQTT_CLOSE",10)){
        memset(auth.mqtt,0,sizeof(auth.mqtt));ph_lan_v4_poll(&endpoint);puts("MQTT_CLOSE OK");
    }
    else if(!strncmp(line,"GRANT ",6)){
        char peer[13],token[33];unsigned scopes;
        bool ok=sscanf(line+6,"%12s %32s %u",peer,token,&scopes)==3&&ph_auth_mqtt_grant(&auth,peer,token,scopes);
        printf("GRANT %s\n",ok?"OK":"DENIED");
    }
    else if(!strncmp(line,"COUNTERS",8))printf("COUNTERS dispatch=%u relay=%u saves=%u retained=%zu\n",dispatches,relay_writes,auth_saves,endpoint.server.response?(size_t)endpoint.server.response->bpos:0);
    else puts("UNKNOWN");fflush(stdout);
}
int main(int argc,char **argv)
{
    assert(argc<=2);unsigned port=argc==2?(unsigned)strtoul(argv[1],NULL,10):0;assert(port<=65535);
    ph_auth_store_empty(&saved_auth);ph_auth_ops auth_ops={0};auth_ops.now=clock_ms;auth_ops.random=random_bytes;auth_ops.password=password;auth_ops.save=save_auth;
    assert(ph_auth_init(&auth,&auth_ops,&saved_auth));
    ph_settings settings;ph_settings_default(&settings);ph_device_ops device_ops={0};device_ops.now=clock_ms;device_ops.save=save_device;device_ops.relay=relay;
    assert(ph_device_init(&device,&device_ops,&settings,"02aabbc12345","v0.0.0-host-lan-v4",42));relay_writes=0;
    ph_lan_v4_ops ops={NULL,clock_ms,free_heap,digest,dispatch};
    assert(ph_lan_v4_init(&endpoint,device.mac,device.boot,&auth,&ops));
    int fd=socket(AF_INET,SOCK_DGRAM,0);assert(fd>=0);struct sockaddr_in local={0};local.sin_family=AF_INET;local.sin_addr.s_addr=htonl(INADDR_LOOPBACK);local.sin_port=htons((uint16_t)port);
    assert(!bind(fd,(struct sockaddr *)&local,sizeof(local)));socklen_t size=sizeof(local);assert(!getsockname(fd,(struct sockaddr *)&local,&size));
    printf("PORT %u ID %s PASSWORD test-pass\n",ntohs(local.sin_port),device.mac);fflush(stdout);
    unsigned char input[LAN_V4_FRAME_MAX+1],frame[LAN_V4_FRAME_MAX];bool running=true;
    while(running){
        fd_set read_set;FD_ZERO(&read_set);FD_SET(fd,&read_set);FD_SET(STDIN_FILENO,&read_set);struct timeval timeout={0,100000};
        int ready=select(fd+1,&read_set,NULL,NULL,&timeout);if(ready<0){if(errno==EINTR)continue;assert(0);}
        if(FD_ISSET(STDIN_FILENO,&read_set)){char line[2*PH_INPUT_MAX+16];if(!fgets(line,sizeof(line),stdin)||!strncmp(line,"STOP",4))running=false;else console(line);}
        if(FD_ISSET(fd,&read_set)){
            struct sockaddr_in remote;size=sizeof(remote);ssize_t n=recvfrom(fd,input,sizeof(input),0,(struct sockaddr *)&remote,&size);
            if(n>0){char ip[INET_ADDRSTRLEN];assert(inet_ntop(AF_INET,&remote.sin_addr,ip,sizeof(ip)));
                void *output=alias_next?(void *)input:(void *)frame;alias_next=false;
                size_t k=ph_lan_v4_feed(&endpoint,input,(size_t)n,ip,ntohs(remote.sin_port),output,sizeof(frame));
                if(k)assert(sendto(fd,output,k,0,(struct sockaddr *)&remote,size)==(ssize_t)k);
            }
        }
        ph_lan_v4_poll(&endpoint);
    }
    ph_lan_v4_revoke(&endpoint);ph_status_cache_release(&device);close(fd);return 0;
}
