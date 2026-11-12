/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "../typed_wire.h"
#include "../mqtt_account.h"
/* Bootstrap is an independent retained MP value, never a JSON string. */
static inline size_t ph_mqtt_discovery_mp(const mqtt_account_t *a,const char *version,
                                        const char *variant,void *bytes,size_t capacity)
{
    ph_writer w;ph_buf buffer;
    if(!a||!version||!variant||!phw_init_fixed(&w,PH_WIRE_MSGPACK,&buffer,bytes,capacity))return 0;
    phw_begin(&w,NULL,false);
    phw_int(&w,"schema",2);phw_text(&w,"model",a->model);
    phw_text(&w,"device_id",a->mac);phw_text(&w,"variant",variant);
    phw_text(&w,"firmware_version",version);phw_text(&w,"protocol","tmcp");
    phw_int(&w,"protocol_version",2);phw_text(&w,"version","v2");
    phw_int(&w,"wire_version",1);phw_begin(&w,"wire_formats",true);
    phw_text(&w,NULL,"msgpack");phw_end(&w);phw_text(&w,"default_wire_format","msgpack");
    phw_end(&w);return phw_finish_fixed(&w);
}
