/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "factory_mac.h"
#include "../../hal/hal_ota.h"
#include <stdlib.h>
static char host_name[32];
int PH_BL_FactoryMac(unsigned char out[6]) {
    unsigned char *sector=malloc(PH_S30_RF_SIZE);if(!sector)return 0;
    int rc=HAL_FlashRead((char*)sector,PH_S30_RF_SIZE,PH_S30_RF_ADDRESS);
    bool ok=rc==0&&ph_factory_mac(sector,PH_S30_RF_SIZE,out);
    free(sector);
    if(ok)ph_mac_host_name(host_name,out);
    return ok;
}
const char *PH_BL_HostName(void) {
    if(!*host_name){unsigned char mac[6];PH_BL_FactoryMac(mac);}
    /* Never let the SDK synthesize an OpenBK name from a default MAC. */
    return *host_name?host_name:"iotcloud-s30-v1b";
}
