#include "bridge.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned factory,extensions;
static char sent[1536];static size_t sent_size;static bool send_ok=true;
static bool send(void *ctx,const char *p,size_t n){(void)ctx;assert(n<=sizeof(sent));if(!send_ok)return false;memcpy(sent,p,n);sent_size=n;return true;}
static void old(void *ctx,const char *p,size_t n){(void)ctx;assert(n==8&&!memcmp(p,"version?",8));factory++;}
static void ext(void *ctx,const void *p,size_t n){(void)ctx;assert(n==3&&!memcmp(p,"abc",3));extensions++;}
int main(void){
 bry_bridge b={.factory=old,.extension=ext};bry_bridge_init(&b);
 const char *request="version?\r\n";for(unsigned i=0;i<strlen(request);i++)bry_bridge_feed(&b,request+i,1);assert(factory==1);
 char frame[1536];size_t n=screen_link_encode(frame,sizeof(frame),123,1,"abc",3);bry_bridge_feed(&b,frame,n);assert(!extensions);
 assert(bry_bridge_negotiate(&b,1,123));for(size_t i=0;i<n;i++)bry_bridge_feed(&b,frame+i,1);assert(extensions==1);
 bry_bridge_feed(&b,frame,n);assert(extensions==1);
 n=screen_link_encode(frame,sizeof(frame),124,2,"abc",3);bry_bridge_feed(&b,frame,n);assert(extensions==1);
 assert(!bry_bridge_negotiate(&b,2,123));n=screen_link_encode(frame,sizeof(frame),123,3,"abc",3);bry_bridge_feed(&b,frame,n);assert(extensions==1);
 char garbage[1800];memset(garbage,'x',sizeof(garbage));bry_bridge_feed(&b,garbage,sizeof(garbage));bry_bridge_feed(&b,"\r\nversion?\r\n",12);assert(factory==2);
 /* Use the real offer/ACK path: an owner test with a mocked capability mask
  * cannot catch a bridge that silently drops a newly implemented service. */
 b.send=send;screen_caps peer;
 const uint32_t supported=SCREEN_CAP_KNOWN&~SCREEN_CAP_FLASH_INFO;
 n=screen_caps_offer(&peer,frame,sizeof(frame),456,SCREEN_CAP_KNOWN);
 bry_bridge_feed(&b,frame,n);
 assert(b.extended&&b.capabilities==supported&&(b.capabilities&SCREEN_CAP_MODULE_CONTROL));
 assert(screen_caps_accept(&peer,sent,sent_size-2)&&peer.ready&&peer.capabilities==supported);
 unsigned char command[12]={0x2b,1,1,0,1};
 assert(bry_bridge_send(&b,command,sizeof(command),SCREEN_CAP_MODULE_CONTROL));
 unsigned char decoded[12];size_t length=sizeof(decoded);uint32_t session,sequence;
 assert(screen_link_decode(sent,sent_size-2,&session,&sequence,decoded,&length));
 assert(session==456&&sequence==1&&length==sizeof(command)&&!memcmp(command,decoded,length));
 bry_bridge_reset(&b);send_ok=false;
 n=screen_caps_offer(&peer,frame,sizeof(frame),457,supported);bry_bridge_feed(&b,frame,n);
 assert(!b.extended&&!b.capabilities&&!bry_bridge_send(&b,command,sizeof(command),SCREEN_CAP_MODULE_CONTROL));
 send_ok=true;bry_bridge_feed(&b,frame,n);assert(b.extended&&b.capabilities==supported);
 /* A retained MCU session must recover even when the boot reset was lost. */
 bry_bridge_reset(&b);sent_size=0;
 bry_bridge_feed(&b,"screen_ext invalid\r\n",20);assert(!sent_size);
 n=screen_link_encode(frame,sizeof(frame),457,1,"abc",3);
 bry_bridge_feed(&b,frame,n);
 const char reset[]="\r\n+screen_link_reset:1\r\n";
 assert(sent_size==sizeof(reset)-1&&!memcmp(sent,reset,sent_size));
 assert(!b.extended&&extensions==1); /* Stale payload was not delivered. */
 n=screen_caps_offer(&peer,frame,sizeof(frame),458,supported);
 bry_bridge_feed(&b,frame,n);
 assert(b.extended&&b.session==458&&screen_caps_accept(&peer,sent,sent_size-2));
 puts("bridge: stock fallback, chunking, replay, session and overflow recovery passed");
}
