/* Product functions are extracted unchanged by test_m1_config_recovery.py.
 * The existing Flash fixture models partitions; it is not a physical device. */
static unsigned acknowledgements;
static uint8_t last_ack[12];
static bool transmit(const uint8_t *frame)
{
    assert(sdk_mutations() && m1_state.reboot_pending);
    memcpy(last_ack,frame,sizeof(last_ack)); acknowledgements++; return true;
}
/* RESET_FRAME */

static void foreign_boot(void)
{
    config_recovery=config_commit_uncertain=false;
    memset(&m1_state,0,sizeof(m1_state)); acknowledgements=0;
    sdk_prepare(); app_config=sdk_application(); m1_context=sdk_context();
    boot_gate(); assert(device_config_recovery() && !sdk_mutations());
}
static void cold_boot(const app_storage_t *expected)
{
    config_recovery=config_commit_uncertain=false;
    memset(&m1_state,0,sizeof(m1_state));
    assert(sdk_cold_read()==0); boot_gate();
    assert(!device_config_recovery() && !sdk_mutations());
    assert(!memcmp(expected,app_config,sizeof(*expected)));
    assert(app_config->magic==M1_CONFIG_MAGIC && app_config->version==M1_CONFIG_VERSION);
}
int main(void)
{
    uint8_t legacy[20]={0x23,0x18},network[20]={0x23,0x72,1,1,9},full[20]={0x23,0x72,1,2,10};
    legacy[19]=network[19]=full[19]=0x21;
    foreign_boot();
    app_storage_t before=*(app_storage_t *)app_config;
    reset_receipt_t receipt={0};
    assert(!m1_reset(false));
    assert(reset_frame(&receipt,legacy) && !receipt.committed);
    assert(reset_frame(&receipt,network) && !receipt.committed);
    assert(!acknowledgements && !sdk_mutations() && !m1_state.reboot_pending && device_config_recovery());
    assert(!memcmp(&before,app_config,sizeof(before)));

    sdk_fail_next_read();
    assert(reset_frame(&receipt,full) && !receipt.committed);
    assert(!acknowledgements && !sdk_mutations() && !m1_state.reboot_pending && device_config_recovery());
    assert(!memcmp(&before,app_config,sizeof(before)));
    sdk_clear_faults();
    assert(reset_frame(&receipt,full) && receipt.committed);
    assert(acknowledgements==1 && sdk_mutations() && m1_state.reboot_pending && !device_config_recovery());
    assert(last_ack[1]==0x72 && last_ack[3]==2 && last_ack[4]==10);
    unsigned writes=sdk_mutations();
    assert(reset_frame(&receipt,full) && acknowledgements==2 && sdk_mutations()==writes);
    app_storage_t expected=*(app_storage_t *)app_config;
    assert(!app_config->ssid[0] && !app_config->wifi_password[0] && !app_config->web_password[0]);
    cold_boot(&expected);

    // In a supported layout, network reset still preserves MQTT settings.
    strcpy(app_config->ssid,"network"); strcpy(app_config->wifi_password,"password");
    strcpy(app_config->web_password,"web-password"); strcpy(app_config->mqtt_host,"broker");
    assert(sdk_update(m1_context)==0); sdk_clear_faults();
    expected=*(app_storage_t *)app_config;
    expected.base.ssid[0]=expected.base.wifi_password[0]=expected.base.web_password[0]=0;
    receipt=(reset_receipt_t){0}; acknowledgements=0;
    assert(reset_frame(&receipt,legacy) && receipt.committed && acknowledgements==1);
    assert(last_ack[1]==0x16 && !strcmp(app_config->mqtt_host,"broker"));
    cold_boot(&expected);

    foreign_boot(); sdk_latch_uncertain();
    m1_config_t candidate=*app_config;
    strcpy(candidate.web_password,"adminfjr");
    before=*(app_storage_t *)app_config;
    assert(!m1_config_save(&candidate));
    assert(!memcmp(&before,app_config,sizeof(before)) && !sdk_mutations());
    assert(!m1_reset(false) && !m1_reset(true));
    assert(device_config_recovery() && !sdk_mutations() && !m1_state.reboot_pending);
    puts("M1 actual reset/UART + pinned SDK: recovery network reset rejected without ACK; full clear failure retry; persisted cold boot; normal network reset; uncertain commit guard PASS");
}
