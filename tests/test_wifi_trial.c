#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
typedef struct { char ip[16]; } IPStatusTypedef;
typedef struct { char ssid[33], wifi_password[65], name[64]; } config_t;
static config_t stored;
static struct { bool wifi_trial_active, ota_active, reboot_pending; char wifi_trial_status[24]; } state;
static int mutex, writes;
static bool fail;
static void mico_rtos_lock_mutex(int *m) { (void)m; }
static void mico_rtos_unlock_mutex(int *m) { (void)m; }
static void get(config_t *c) { *c = stored; }
static bool save(const config_t *c) { writes++; if (fail) return false; stored = *c; return true; }
#define WIFI_PREFIX test
#define WIFI_CONFIG config_t
#define WIFI_STATE state
#define WIFI_LOCK mutex
#define WIFI_GET get
#define WIFI_SAVE save
#include "../common/wifi_trial.inc"
int main(void)
{
    config_t candidate = {.ssid="new", .wifi_password="newpassword"}, work;
    strcpy(stored.ssid,"old"); strcpy(stored.wifi_password,"oldpassword");
    assert(test_wifi_trial(&candidate));
    assert(!test_wifi_trial(&candidate));
    assert(!writes && !strcmp(stored.ssid,"old"));
    get(&work); assert(wifi_trial_prepare(&work, UINT32_MAX-100));
    assert(!strcmp(work.ssid,"new") && !strcmp(stored.ssid,"old"));
    assert(!wifi_trial_finish(false,200));
    assert(!wifi_trial_finish(true,250)); /* stale pre-trial IP is insufficient */
    IPStatusTypedef lease={.ip="192.168.1.2"};wifi_trial_dhcp(&lease,NULL);
    strcpy(stored.name,"concurrent name save");
    assert(wifi_trial_finish(true,300)==1);
    assert(writes==1 && !strcmp(stored.ssid,"new") && !strcmp(stored.name,"concurrent name save"));
    assert(!state.wifi_trial_active && !strcmp(state.wifi_trial_status,"success"));
    strcpy(candidate.ssid,"wrong password");
    assert(test_wifi_trial(&candidate)); get(&work); wifi_trial_prepare(&work,1000);
    assert(!wifi_trial_finish(false,30999));
    assert(wifi_trial_finish(false,31000)==-1);
    assert(writes==1 && !strcmp(stored.ssid,"new"));
    assert(!strcmp(state.wifi_trial_status,"timeout_rollback"));
    assert(test_wifi_trial(&candidate)); get(&work); wifi_trial_prepare(&work,0);
    wifi_trial_dhcp(&lease,NULL);fail=true; assert(wifi_trial_finish(true,1)==-1);
    assert(!strcmp(state.wifi_trial_status,"storage_failure") && !strcmp(stored.ssid,"new"));
    state.ota_active=true; assert(!test_wifi_trial(&candidate));
    state.ota_active=false; state.reboot_pending=true; assert(!test_wifi_trial(&candidate));
    puts("Wi-Fi RAM candidate, concurrent save merge, wraparound, timeout and storage rollback passed");
}
