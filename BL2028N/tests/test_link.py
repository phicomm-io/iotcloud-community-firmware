#!/usr/bin/env python3
"""Production STA detector and the patched SDK pbuf ownership boundary."""
import argparse
from pathlib import Path
import shutil
import subprocess

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
SDK = Path.home()/'.cache/phicomm/openbeken/bk7231n-upstream'
REL = Path('platforms/bk7231n/bk7231n_os/beken378/func/lwip_intf/lwip-2.1.3/port/ethernetif.c')


def function(text, signature):
    start = text.index(signature)
    end = text.index('{', start)+1
    depth = 1
    while depth:
        depth += (text[end] == '{')-(text[end] == '}')
        end += 1
    return text[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run-dir', type=Path, required=True)
    args = parser.parse_args()
    run = args.run_dir.resolve()
    run.mkdir(parents=True, exist_ok=True)
    flags = ['clang', '-std=gnu99', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
             '-Wno-misleading-indentation', '-fsanitize=address,undefined', '-fno-omit-frame-pointer']

    def command(argv, log):
        with (run/log).open('w') as stream:
            subprocess.run(list(map(str, argv)), stdout=stream, stderr=subprocess.STDOUT, check=True, timeout=30)

    command(flags+['-DPH_HOST_TEST', '-DPH_LINK_TEST', '-I', HERE, HERE/'link.c',
                   ROOT/'src/ph_link.c', '-o', run/'link'], 'compiler.log')
    for scenario in range(10):
        command([run/'link', scenario], f'case-{scenario}.log')

    fixture = run/'sdk-fixture'
    target = fixture/REL
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(SDK/REL, target)
    with (run/'patch.log').open('w') as log:
        subprocess.run(['patch', '-p1', '--batch', '-i', str(ROOT/'patches/link-rx.patch')],
                       cwd=fixture, stdout=log, stderr=subprocess.STDOUT, check=True)
    stubs = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define SIZEOF_ETH_HDR 14
#define ERR_OK 0
#define ETHTYPE_IP 0x0800
#define ETHTYPE_ARP 0x0806
#define ETHTYPE_EAPOL 0x888e
#define LWIP_DEBUGF(...) ((void)0)
#define htons(x) __builtin_bswap16(x)
typedef int err_t;
struct eth_hdr {struct {unsigned char addr[6];} dest,src;uint16_t type;};
struct pbuf {unsigned len;void *payload;};
struct netif {int (*input)(struct pbuf *,struct netif *);};
static unsigned observed,group_count,errors,frees,eapol;
static int error;
static void pbuf_free(struct pbuf *p){free(p->payload);free(p);frees++;}
static int input(struct pbuf *p,struct netif *n){(void)n;if(error)return -1;pbuf_free(p);return ERR_OK;}
static struct netif station={input};
static struct netif *rwm_mgmt_get_vif2netif(uint8_t iface){return iface==0?&station:NULL;}
static void PH_BL_LinkRx(void *n,bool group,bool accepted){assert(n==&station);observed++;group_count+=group;errors+=!accepted;}
static void ke_l2_packet_tx(void *p,unsigned n,int iface){assert(p&&n==42&&iface==0);eapol++;}
'''
    cases = r'''
static void frame(unsigned type,bool group,int iface,unsigned len){
 struct pbuf *p=calloc(1,sizeof(*p));p->len=len;p->payload=calloc(1,42);
 struct eth_hdr *eth=p->payload;eth->type=htons(type);eth->dest.addr[0]=group?0xff:0x0c;
 ethernetif_input(iface,p);
}
int main(void){
 frame(ETHTYPE_IP,false,0,42);assert(observed==1&&!group_count&&!errors&&frees==1);
 frame(ETHTYPE_ARP,true,0,42);assert(observed==2&&group_count==1&&frees==2);
 error=1;frame(ETHTYPE_IP,false,0,42);assert(observed==3&&errors==1&&frees==3);
 frame(ETHTYPE_IP,false,1,42);frame(ETHTYPE_IP,false,0,14);assert(observed==3&&frees==5);
 frame(ETHTYPE_EAPOL,false,0,42);assert(observed==3&&frees==6&&eapol==1);
 puts("PASS: SDK RX hook classifies before input can free pbuf; failed admission, short/unknown netif and EAPOL retain ownership");
}
'''
    source = run/'sdk-input.c'
    source.write_text(stubs+function(target.read_text(), 'void\nethernetif_input(int iface, struct pbuf *p)')+cases)
    command(flags+[source, '-o', run/'sdk-input'], 'sdk-compiler.log')
    command([run/'sdk-input'], 'sdk-input.log')
    print('PASS: 10 STA link scenarios and the actual patched SDK input, ASan/UBSan')


if __name__ == '__main__':
    main()
