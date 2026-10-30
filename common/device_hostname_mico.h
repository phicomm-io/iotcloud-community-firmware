/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "device_hostname.h"
/* The mocIP symbol uses one argument. Give its C declaration a separate name
 * so newlib's POSIX sethostname(const char *, size_t) cannot conflict with it. */
extern char *ph_mico_sethostname(char *name) __asm__("sethostname");
static inline bool ph_device_hostname_mico(const char *model,const char *id)
{
    /* The network stack may retain the pointer across DHCP renewals. */
    static char hostname[40];
    if(!ph_device_hostname(hostname,sizeof(hostname),model,id))return false;
    ph_mico_sethostname(hostname);
    return true;
}
