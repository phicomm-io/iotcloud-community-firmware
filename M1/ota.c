#include "../common/watchdog_flash.h"
#include "m1.h"
#include <ctype.h>
#include "../common/ota/fetch.h"
#include "../common/ota/mode.h"
static bool download_running;
#include "../common/ota/signed_ota.h"
static signed_ota signed_session;
static bool unsigned_session;
static uint32_t wire_expected, wire_received;

static uint32_t staged, expected;
static uint16_t transport_crc;
static const uint8_t kernel_md5[16] = {
    0xcd,0x36,0x18,0xb8,0x1c,0xa8,0xcd,0x26,0xb4,0x3c,0xac,0xa8,0x85,0xfb,0xc9,0xbf
};

static uint32_t le32(const uint8_t *p)
{ return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t le16(const uint8_t *p) { return p[0] | ((uint16_t)p[1] << 8); }

void m1_ota_status(const char *state, const char *error, unsigned progress)
{
    mico_rtos_lock_mutex(&m1_lock);
    snprintf(m1_state.ota_state, sizeof(m1_state.ota_state), "%s", state);
    snprintf(m1_state.ota_error, sizeof(m1_state.ota_error), "%s", error ? error : "");
    m1_state.ota_progress = progress;
    mico_rtos_unlock_mutex(&m1_lock);
}

static bool reserve(void)
{
    if(!ota_job_claim())return false;
    mico_rtos_lock_mutex(&m1_lock);
    bool free = !m1_state.ota_active && !m1_state.reboot_pending && !m1_state.wifi_trial_active;
    if (free) {
        snprintf(m1_state.ota_state,sizeof(m1_state.ota_state),"authenticating");
        m1_state.ota_error[0]=0;m1_state.ota_progress=0;
        m1_state.ota_active=true;m1_state.ota_signature_verified=false;
        m1_state.ota_image_verified=false;m1_state.ota_key_id=0;m1_state.ota_image_length=0;
        m1_state.ota_release_counter[0]=0;m1_state.ota_version[0]=0;
    }
    mico_rtos_unlock_mutex(&m1_lock);
    if(!free)ota_job_release();
    return free;
}

void m1_ota_abort(const char *why)
{
    watchdog_ota_end();
    m1_ota_status("failed", why, 0);
    mico_rtos_lock_mutex(&m1_lock); m1_state.ota_active = false; mico_rtos_unlock_mutex(&m1_lock);
    if(!download_running && ota_job_active())ota_job_release();
}

static bool prepare(uint32_t length)
{
    mico_logic_partition_t *part = MicoFlashGetInfo(MICO_PARTITION_OTA_TEMP);
    if (!part || length < M1_APP_OFFSET + 128 + 16 || length > M1_APP_OFFSET + M1_APP_LIMIT + 16 ||
        length > part->partition_length) {
        m1_ota_abort("Invalid OTA length or partition capacity"); return false;
    }
    expected = length; staged = 0; transport_crc = 0;
    m1_ota_status("receiving", NULL, 0);
    if (!watchdog_erase_ota(part->partition_length)) {
        m1_ota_abort("OTA flash erase failed"); return false;
    }
    return true;
}

static bool native_write(const uint8_t *data, size_t n)
{
    if (!n || n > expected - staged) { m1_ota_abort("OTA body exceeds advertised length"); return false; }
    while (n) {
        unsigned take = n > 512 ? 512 : n;
        uint32_t offset = staged;
        if (MicoFlashWrite(MICO_PARTITION_OTA_TEMP, &offset, (uint8_t *)data, take) != kNoErr ||
            !watchdog_ota_progress()) {
            m1_ota_abort("OTA flash write failed"); return false;
        }
        transport_crc = m1_crc16(transport_crc, data, take);
        staged += take; data += take; n -= take;
    }
    m1_ota_status("receiving", NULL, (unsigned)((uint64_t)staged * 100 / expected));
    return true;
}

static bool signed_erase(void *ctx)
{ (void)ctx; return prepare(signed_session.image_length); }
static bool signed_write(void *ctx, uint32_t offset, const uint8_t *data, size_t n)
{ (void)ctx; if (offset != staged) return false; return native_write(data,n); }
static bool signed_read(void *ctx, uint32_t offset, uint8_t *data, size_t n)
{ (void)ctx; return watchdog_read_ota(offset,data,n); }
bool m1_ota_begin(uint32_t length)
{
    if (!reserve()) return false;
    if(!ota_job_enter()){m1_ota_abort("Cannot stop linkage worker");return false;}
    unsigned_session=false; wire_expected=length; wire_received=0;
    mico_logic_partition_t *part=MicoFlashGetInfo(MICO_PARTITION_OTA_TEMP);
    signed_ota_io io={signed_erase,signed_write,signed_read,NULL};
    if (!part || !signed_ota_begin(&signed_session,length,part->partition_length,2,1,io)) {
        m1_ota_abort("Invalid signed package length"); return false;
    }
    m1_ota_status("authenticating",NULL,0); return true;
}
bool m1_ota_begin_unsigned(uint32_t length)
{
    if (!reserve()) return false;
    if(!ota_job_enter()){m1_ota_abort("Cannot stop linkage worker");return false;}
    unsigned_session=true; wire_expected=length; wire_received=0;
    if (!watchdog_ota_begin()) { m1_ota_abort("Watchdog unavailable"); return false; }
    bool ok = prepare(length);
    watchdog_ota_end();
    return ok;
}
bool m1_ota_write(const uint8_t *data, size_t n)
{
    if(ota_job_cancelled()){m1_ota_abort("Cancelled");return false;}
    if (!watchdog_ota_begin()) { m1_ota_abort("Watchdog unavailable"); return false; }
    if (n>wire_expected-wire_received ||
        !(unsigned_session ? native_write(data,n) : signed_ota_write(&signed_session,data,n))) {
        m1_ota_abort("Invalid signature, package or Flash write"); return false;
    }
    if(!unsigned_session && signed_session.authenticated) {
        const uint8_t *h=signed_session.envelope;
        uint64_t counter=(uint64_t)le32(h+24)|((uint64_t)le32(h+28)<<32);
        mico_rtos_lock_mutex(&m1_lock);
        m1_state.ota_signature_verified=true;m1_state.ota_key_id=le32(h+12);
        m1_state.ota_image_length=le32(h+20);
        ota_counter_decimal(counter,m1_state.ota_release_counter);
        memcpy(m1_state.ota_version,h+64,32);
        mico_rtos_unlock_mutex(&m1_lock);
    }
    wire_received+=n;
    watchdog_ota_end();
    return true;
}
static bool read_at(uint32_t offset, void *data, unsigned length)
{
    if (offset > expected || length > expected - offset) return false;
    return watchdog_read_ota(offset,data,length);
}

static bool md5_range(uint32_t start, uint32_t end, uint8_t digest[16])
{
    md5_context md5; InitMd5(&md5);
    uint8_t data[512];
    while (start < end) {
        unsigned n = end - start > sizeof(data) ? sizeof(data) : end - start;
        if (!read_at(start, data, n)) return false;
        Md5Update(&md5, data, n); start += n;
    }
    Md5Final(&md5, digest); return true;
}

bool m1_ota_finish(void)
{
    if (!watchdog_ota_begin()) { m1_ota_abort("Watchdog unavailable"); return false; }
    const char *error = NULL;
    if (!unsigned_session && !signed_ota_finish(&signed_session)) {
        m1_ota_abort("Signed image readback SHA-256 mismatch or incomplete package"); return false;
    }
    mico_rtos_lock_mutex(&m1_lock);m1_state.ota_image_verified=!unsigned_session;
    mico_rtos_unlock_mutex(&m1_lock);
    uint8_t header[36], digest[16], tail[16], data[512];
    m1_ota_status("validating", NULL, 100);
    if (staged != expected) { error = "Truncated OTA body"; goto fail; }
    if (!read_at(M1_APP_OFFSET, header, sizeof(header))) { error = "OTA header read failed"; goto fail; }
    uint32_t payload = le32(header), pid = le32(header + 28);
    if (payload < 120 || payload > M1_APP_LIMIT - 8 ||
        expected != M1_APP_OFFSET + 8 + payload + 16 || le32(header + 8) != 0xc89346 ||
        le32(header + 16) != 3 || le16(header + 4) != le16(header + 6)) {
        error = "Invalid application header or ABI"; goto fail;
    }
    if (pid < M1_APP_CPU_ADDRESS + 8 || pid - M1_APP_CPU_ADDRESS > 8 + payload - sizeof(M1_PRODUCT)) {
        error = "Product pointer outside application"; goto fail;
    }
    char product[sizeof(M1_PRODUCT)];
    if (!read_at(M1_APP_OFFSET + pid - M1_APP_CPU_ADDRESS, product, sizeof(product)) ||
        memcmp(product, M1_PRODUCT, sizeof(product))) { error = "Not an M1 EMW3080 image"; goto fail; }
    uint16_t crc = 0;
    uint32_t position = M1_APP_OFFSET + 8, end = expected - 16;
    while (position < end) {
        unsigned n = end - position > sizeof(data) ? sizeof(data) : end - position;
        if (!read_at(position, data, n)) { error = "OTA flash read failed"; goto fail; }
        crc = m1_crc16(crc, data, n); position += n;
    }
    if (crc != le16(header + 4)) { error = "Application CRC mismatch"; goto fail; }
    if (!md5_range(0, M1_APP_OFFSET, digest) || memcmp(digest, kernel_md5, 16)) {
        error = "Incompatible MiCO kernel"; goto fail;
    }
    if (!md5_range(0, expected - 16, digest) || !read_at(expected - 16, tail, 16) || memcmp(digest, tail, 16)) {
        error = "OTA MD5 mismatch"; goto fail;
    }
    if(!ota_job_commit()){error="Cancelled";goto fail;}
    if (mico_ota_switch_to_new_fw(expected, transport_crc) != kNoErr) {
        error = "Failed to persist OTA boot metadata"; goto fail;
    }
    mico_rtos_lock_mutex(&m1_lock);
    strcpy(m1_state.ota_state, "ready"); m1_state.ota_active = false;
    m1_state.reboot_pending = true; m1_state.reboot_at = mico_rtos_get_time() + 1500;
    mico_rtos_unlock_mutex(&m1_lock);
    watchdog_ota_end();
    return true;
fail:
    m1_ota_abort(error); return false;
}

static bool fetch_begin(void *ctx,uint32_t length){
    (void)ctx;unsigned_session=false;wire_expected=length;wire_received=0;
    mico_logic_partition_t *part=MicoFlashGetInfo(MICO_PARTITION_OTA_TEMP);
    signed_ota_io io={signed_erase,signed_write,signed_read,NULL};
    return part && signed_ota_begin(&signed_session,length,part->partition_length,2,1,io);
}
static bool fetch_write(void *ctx,uint32_t offset,const uint8_t *data,size_t n){
    (void)ctx;return offset==wire_received && m1_ota_write(data,n);
}
static void download_worker(mico_thread_arg_t arg){
    char *url=(char *)arg;const char *error="Cannot stop linkage worker";bool ok=false;
    if(ota_job_enter()){
        ok=ota_fetch(url,M1_APP_OFFSET+M1_APP_LIMIT+16+OTA_ENVELOPE_SIZE,fetch_begin,fetch_write,NULL,&error);
        if(ok)ok=m1_ota_finish();
    }
    if(!ok && error)m1_ota_abort(error);
    free(url);download_running=false;
    if(!ok)ota_job_release();
    mico_rtos_delete_thread(NULL);
}

bool m1_ota_download(const char *url)
{
    if (!ota_url_valid(url) || !reserve()) return false;
    char *copy = malloc(strlen(url) + 1);
    if (!copy) { m1_ota_abort("No memory for download"); return false; }
    strcpy(copy, url); download_running=true; m1_ota_status("connecting", NULL, 0);
    if (mico_rtos_create_thread(NULL, MICO_APPLICATION_PRIORITY, "m1_ota", download_worker, 6144,
                                (mico_thread_arg_t)copy) != kNoErr) {
        download_running=false; free(copy); m1_ota_abort("Cannot start OTA worker"); return false;
    }
    return true;
}
