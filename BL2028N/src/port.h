/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
void PH_BL_Init(int safe_mode);
void PH_BL_Tick(void);
void PH_BL_StartHTTP(void);
int PH_BL_FactoryMac(unsigned char out[6]);
int PH_BL_WebPasswordSet(const char *);
/* SDK adapter: bounded copy/dirty marker, without recursive auth callbacks. */
void PH_BL_SetWebPasswordMemory(const char *);
int PH_BL_MqttTlsEnabled(void);
const char *PH_BL_APName(void);
const char *PH_BL_HostName(void);
