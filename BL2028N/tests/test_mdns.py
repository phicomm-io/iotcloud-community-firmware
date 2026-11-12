#!/usr/bin/env python3
"""S30 mDNS adapter and actual pinned SDK checked-init failure paths."""
import argparse
from pathlib import Path
import shutil
import subprocess

ROOT=Path(__file__).resolve().parents[2]
HERE=Path(__file__).resolve().parent
SDK=Path.home()/'.cache/phicomm/openbeken/bk7231n-upstream'
SDK_REL=Path('platforms/bk7231n/bk7231n_os/beken378/func/lwip_intf/lwip-2.1.3/src')

def function(text,signature):
    start=text.index(signature);begin=text.index('{',start);depth=1;end=begin+1
    while depth:
        if text[end]=='{':depth+=1
        elif text[end]=='}':depth-=1
        end+=1
    return text[start:end]

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--run-dir',type=Path,required=True);args=parser.parse_args();run=args.run_dir.resolve();run.mkdir(parents=True,exist_ok=True)
    flags=[shutil.which('clang') or 'cc','-std=gnu99','-g','-O1','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-fsanitize=address,undefined','-fno-omit-frame-pointer']
    def command(argv,name):
        with (run/name).open('w') as log:subprocess.run([str(v) for v in argv],stdout=log,stderr=subprocess.STDOUT,check=True)
    command(flags+['-DPH_HOST_TEST','-DPH_MDNS_TEST','-DPH_S30_LAN_V4','-I',HERE,HERE/'mdns.c',ROOT/'BL2028N/src/ph_mdns.c','-o',run/'mdns-adapter'],'adapter-compiler.log')
    command([run/'mdns-adapter'],'adapter.log')
    copy=run/'sdk-fixture'
    for rel in ('apps/mdns/mdns.c','include/lwip/apps/mdns.h'):
        target=copy/SDK_REL/rel;target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes((SDK/SDK_REL/rel).read_bytes())
    with (run/'sdk-patch.log').open('w') as log:subprocess.run(['patch','-p1','--batch','-i',str(ROOT/'BL2028N/patches/mdns-init-checked.patch')],cwd=copy,stdout=log,stderr=subprocess.STDOUT,check=True)
    source=(copy/SDK_REL/'apps/mdns/mdns.c').read_text()
    prelude=r'''
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
typedef int err_t;
typedef uint8_t u8_t;
#define ERR_OK 0
#define ERR_MEM -1
#define ERR_VAL -2
#define ERR_USE -8
#define LWIP_MULTICAST_TX_OPTIONS 1
#define MDNS_RESP_USENETIF_EXTCALLBACK 0
#define MDNS_TTL 255
#define IPADDR_TYPE_ANY 0
#define IP_ANY_TYPE NULL
#define LWIP_IANA_PORT_MDNS 5353
#define LWIP_UNUSED_ARG(v) ((void)(v))
#define LWIP_ASSERT(message,value) assert(value)
#define LWIP_ASSERT_CORE_LOCKED() ((void)0)
#define MEMCPY memcpy
struct mdns_service {struct {uint8_t name[256];uint16_t length;uint8_t skip_compression;} txtdata;};
struct udp_pcb {int ttl;};
static struct udp_pcb pcb,*mdns_pcb;
static unsigned fault,allocations,removals,binds,receivers,clients;
static unsigned char mdns_netif_client_id;
static struct udp_pcb *udp_new_ip_type(int type){assert(!type);allocations++;return fault==1?NULL:&pcb;}
static void udp_set_multicast_ttl(struct udp_pcb *p,int ttl){assert(p==&pcb);p->ttl=ttl;}
static err_t udp_bind(struct udp_pcb *p,const void *ip,unsigned port){assert(p==&pcb&&!ip&&port==5353);binds++;return fault==2?ERR_USE:ERR_OK;}
static void udp_remove(struct udp_pcb *p){assert(p==&pcb);removals++;}
static void mdns_recv(void){}
static void udp_recv(struct udp_pcb *p,void (*fn)(void),void *arg){assert(p==&pcb&&fn==mdns_recv&&!arg);receivers++;}
static unsigned char netif_alloc_client_data_id(void){clients++;return 0;}
'''
    main=r'''
int main(void){
 fault=1;assert(mdns_resp_init_checked()==ERR_MEM&&!mdns_pcb&&!binds&&!clients);
 fault=2;assert(mdns_resp_init_checked()==ERR_USE&&!mdns_pcb&&removals==1&&!clients);
 fault=0;assert(mdns_resp_init_checked()==ERR_OK&&mdns_pcb==&pcb&&clients==1&&receivers==1&&pcb.ttl==255);
 unsigned before=allocations;assert(mdns_resp_init_checked()==ERR_OK&&allocations==before);mdns_resp_init();assert(allocations==before&&clients==1&&mdns_netif_client_id==0);
 struct {uint32_t before;struct mdns_service service;uint32_t after;} guarded={0};guarded.before=0x12345678;guarded.after=0x87654321;
 struct mdns_service *s=&guarded.service;char bytes[255];memset(bytes,'x',sizeof(bytes));
 assert(mdns_resp_add_service_txtitem(s,bytes,72)==ERR_OK&&s->txtdata.length==73&&s->txtdata.name[0]==72&&!memcmp(s->txtdata.name+1,bytes,72));
 s->txtdata.length=0;assert(mdns_resp_add_service_txtitem(s,bytes,255)==ERR_OK&&s->txtdata.length==256&&s->txtdata.name[0]==255);
 struct mdns_service frozen=*s;assert(mdns_resp_add_service_txtitem(s,bytes,1)==ERR_VAL&&!memcmp(s,&frozen,sizeof(frozen)));
 s->txtdata.length=0;assert(mdns_resp_add_service_txtitem(s,NULL,1)==ERR_VAL&&!s->txtdata.length);
 assert(mdns_resp_add_service_txtitem(s,NULL,0)==ERR_OK&&s->txtdata.length==1&&!s->txtdata.name[0]);
 s->txtdata.length=257;assert(mdns_resp_add_service_txtitem(s,bytes,1)==ERR_VAL&&s->txtdata.length==257);
 assert(guarded.before==0x12345678&&guarded.after==0x87654321);
 puts("actual pinned SDK: PCB OOM/bind cleanup/retry/idempotence/legacy API; TXT 72/255/aggregate overflow/NULL/canaries PASS");return 0;
}
'''
    fixture=run/'sdk-checked.c';fixture.write_text(prelude+'\n'+function(source,'err_t\nmdns_resp_init_checked(')+'\n'+function(source,'void\nmdns_resp_init(')+'\n'+function(source,'err_t\nmdns_resp_add_service_txtitem(')+'\n'+main)
    command(flags+[fixture,'-o',run/'sdk-checked'],'sdk-compiler.log');command([run/'sdk-checked'],'sdk.log')
    print(f'PASS: SDK mDNS adapter and pinned checked-init faults. Evidence: {run}')

if __name__=='__main__':main()
