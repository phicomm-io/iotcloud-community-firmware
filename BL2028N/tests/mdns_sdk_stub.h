/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
typedef int err_t;
typedef uint8_t u8_t;
typedef int8_t s8_t;
#define ERR_OK 0
#define ERR_MEM -1
#define ERR_VAL -2
typedef struct {uint32_t addr;} ip4_addr_t;
struct netif {ip4_addr_t ip;bool up,link;};
struct mdns_service {char txt[256];size_t length;};
enum mdns_sd_proto {DNSSD_PROTO_UDP=0,DNSSD_PROTO_TCP=1};
typedef void (*service_get_txt_fn_t)(struct mdns_service *,void *);
#define netif_is_up(n) ((n)->up)
#define netif_is_link_up(n) ((n)->link)
#define netif_ip4_addr(n) (&(n)->ip)
#define ip4_addr_isany_val(ip) (!(ip).addr)
#define ip4_addr_get_u32(ip) ((ip)->addr)
void *net_get_sta_handle(void);
err_t tcpip_callback(void (*)(void *),void *);
err_t mdns_resp_init_checked(void);
err_t mdns_resp_add_netif(struct netif *,const char *,uint32_t);
err_t mdns_resp_remove_netif(struct netif *);
s8_t mdns_resp_add_service(struct netif *,const char *,const char *,enum mdns_sd_proto,
                         uint16_t,uint32_t,service_get_txt_fn_t,void *);
err_t mdns_resp_add_service_txtitem(struct mdns_service *,const char *,u8_t);
