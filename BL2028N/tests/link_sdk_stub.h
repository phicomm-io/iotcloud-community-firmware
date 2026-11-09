/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef int err_t;
typedef struct {uint32_t addr;} ip4_addr_t;
struct netif {ip4_addr_t ip,gateway;bool up,link;};
#define ERR_OK 0
#define ERR_MEM -1
#define netif_is_up(n) ((n)->up)
#define netif_is_link_up(n) ((n)->link)
#define netif_ip4_addr(n) (&(n)->ip)
#define netif_ip4_gw(n) (&(n)->gateway)
#define ip4_addr_isany_val(a) (!(a).addr)
#define ip4_addr_get_u32(p) ((p)->addr)
void *net_get_sta_handle(void);
err_t tcpip_callback(void (*)(void *),void *);
err_t etharp_request(struct netif *,const ip4_addr_t *);
