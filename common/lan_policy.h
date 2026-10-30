/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <string.h>
/* The includer provides ph_auth_context and bool through its platform ABI. */
static inline bool ph_lan_action_allowed(const char *action)
{
    static const char *const allowed[]={
        "status.get","capabilities.get","info.get","diagnostics.get",
        "power.get","filter.get","alarm.get","wake_alarm.get","mcu.version.get",
        "fan.set","fan.mode.set","fan.mode.toggle","light.set","child_lock.set",
        "light_always.set","light_effect.select","light_effect.next",
        "outlet.set","indicator.set","display.set","buzzer.set",
        "wake_alarm.start","wake_alarm.stop",
        "fresh_air.config.get","fresh_air.linkage.get","fresh_air.set",NULL
    };
    if(!action)return false;
    for(unsigned i=0;allowed[i];i++)if(!strcmp(action,allowed[i]))return true;
    return false;
}
/* Hash domain || original request_id. Do not change the existing byte layout. */
static inline void ph_lan_receipt_domain(unsigned char domain[32],const ph_auth_context *context)
{
    memset(domain,0,32);memcpy(domain,"lan-v4",6);memcpy(domain+8,context->peer,13);
    domain[21]=(unsigned char)context->source;domain[22]=context->slot;
    for(unsigned i=0;i<4;i++)domain[24+i]=(unsigned char)(context->generation>>(24-8*i));
}
