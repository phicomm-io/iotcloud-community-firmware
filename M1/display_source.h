/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef M1_DISPLAY_SOURCE_H
#define M1_DISPLAY_SOURCE_H
#include <stdint.h>
#include <stdbool.h>
/* Policy belongs to the Wi-Fi MCU. The display MCU receives only segments,
 * colors and a short lease; none of its original sensor values are replaced. */
static inline bool m1_display_source_selected(unsigned mode,int manual,
                                               uint32_t elapsed,unsigned seconds)
{
    if(manual>=0)return manual!=0;
    if(mode==1)return true;
    return mode==2 && seconds && elapsed%((uint32_t)seconds*2000)<(uint32_t)seconds*1000;
}
static inline void m1_display_source_encode(uint8_t args[7],uint16_t ppm,uint8_t ttl,
                                            uint8_t slot,uint16_t low,uint16_t high)
{
    uint16_t number=ppm;uint8_t decimal=0;
    if(ppm>=10000){number=ppm/100;decimal=2;}
    else if(ppm>=1000){number=ppm/10;decimal=1;}
    args[0]=number>>8;args[1]=number;args[2]=decimal;
    args[3]=ppm>=high?1:ppm>=low?3:2;
    args[4]=ttl;args[5]=slot;args[6]=0;
}
#endif
