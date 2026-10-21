#pragma once
#include <stdbool.h>
/* Thread-context state copies only. Never hold over allocation, JSON, GPIO,
 * network or Flash. The sole nested lock is the OTA/config metadata mutex:
 * status -> OTA, each held only for copies/flags. Never acquire status while
 * holding OTA; OTA callbacks release their metadata lock before any state call. */
bool tc1_status_guard_init(void);
bool tc1_status_guard_ready(void);
void tc1_status_lock(void);
void tc1_status_unlock(void);
#define TC1_STATUS_WRITE(...) do { tc1_status_lock(); __VA_ARGS__; tc1_status_unlock(); } while(0)
/* Caller holds status lock: config reservation shares this synchronization
 * domain so a snapshot cannot race the start of SDK restore/rollback. */
bool tc1_config_changing(void);
/* On success leaves the guard held until tc1_status_unlock(). Config restore,
 * persistence rollback and Wi-Fi trials reject all groups while in progress. */
bool tc1_status_begin(void);
