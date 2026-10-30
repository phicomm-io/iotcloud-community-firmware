/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PH_HA_NUMBER_H
#define PH_HA_NUMBER_H
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
/* Avoid the framework formatter's reduced float precision and unsupported
 * general format. Six fractional places cover meter and accumulated energy. */
static void ph_ha_number(char out[32], double value) {
    if (!isfinite(value) || value<0 || value>1e12) { memcpy(out,"null",5);return; }
    uint64_t whole=(uint64_t)value;
    unsigned long fraction=(unsigned long)((value-(double)whole)*1000000+.5);
    if (fraction>=1000000) { ++whole;fraction=0; }
    snprintf(out,32,"%llu.%06lu",(unsigned long long)whole,fraction);
}
#endif
