/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t unicast_rx,group_rx,input_errors,probes,probe_errors,recoveries;
} ph_link_stats;

/* The LAN owner supplies current STA/maintenance readiness. lwIP operations
 * are serialized in tcpip_callback; no socket or extra task is allocated. */
void PH_BL_LinkPoll(bool ready);
/* SDK RX boundary, after input accepted/rejected an IP/ARP frame. */
void PH_BL_LinkRx(void *interface,bool group,bool accepted);
ph_link_stats PH_BL_LinkSnapshot(void);
/* Native Wi-Fi owner consumes requests outside tcpip_thread. Recheck scan,
 * provisioning and OTA immediately before stopping/reconnecting the station. */
bool PH_BL_LinkTakeRecovery(bool allowed);
