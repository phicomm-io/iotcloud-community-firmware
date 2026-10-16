#include "../common/watchdog_flash.h"
#include "a1.h"
#include <ctype.h>
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

void a1_ota_status(const char *state, const char *error, unsigned progress)
{
    mico_rtos_lock_mutex(&a1_lock);
    snprintf(a1_state.ota_state, sizeof(a1_state.ota_state), "%s", state);
    snprintf(a1_state.ota_error, sizeof(a1_state.ota_error), "%s", error ? error : "");
    a1_state.ota_progress = progress;
    mico_rtos_unlock_mutex(&a1_lock);
}

static bool reserve(void)
{
    if(!a1_upgrade_reserve())return false;
    mico_rtos_lock_mutex(&a1_lock);
    snprintf(a1_state.ota_state,sizeof(a1_state.ota_state),"authenticating");
    a1_state.ota_error[0]=0;a1_state.ota_progress=0;
    a1_state.ota_signature_verified=false;a1_state.ota_image_verified=false;
    a1_state.ota_key_id=0;a1_state.ota_image_length=0;
    a1_state.ota_release_counter[0]=0;a1_state.ota_version[0]=0;
    mico_rtos_unlock_mutex(&a1_lock);return true;
}

void a1_ota_abort(const char *why)
{
    watchdog_ota_end();
    a1_ota_status("failed", why, 0);
    mico_rtos_lock_mutex(&a1_lock); a1_state.ota_active = false; mico_rtos_unlock_mutex(&a1_lock);
}

static bool prepare(uint32_t length)
{
    mico_logic_partition_t *part = MicoFlashGetInfo(MICO_PARTITION_OTA_TEMP);
    if (!part || length < A1_APP_OFFSET + 128 + 16 || length > A1_APP_OFFSET + A1_APP_LIMIT + 16 ||
        length > part->partition_length) {
        a1_ota_abort("Invalid OTA length or partition capacity"); return false;
    }
    expected = length; staged = 0; transport_crc = 0;
    a1_ota_status("receiving", NULL, 0);
    if (!watchdog_erase_ota(part->partition_length)) {
        a1_ota_abort("OTA flash erase failed"); return false;
    }
    return true;
}

static bool native_write(const uint8_t *data, size_t n)
{
    if (!n || n > expected - staged) { a1_ota_abort("OTA body exceeds advertised length"); return false; }
    while (n) {
        unsigned take = n > 512 ? 512 : n;
        uint32_t offset = staged;
        if (MicoFlashWrite(MICO_PARTITION_OTA_TEMP, &offset, (uint8_t *)data, take) != kNoErr ||
            !watchdog_ota_progress()) {
            a1_ota_abort("OTA flash write failed"); return false;
        }
        transport_crc = a1_crc16(transport_crc, data, take);
        staged += take; data += take; n -= take;
    }
    a1_ota_status("receiving", NULL, (unsigned)((uint64_t)staged * 100 / expected));
    return true;
}

static bool signed_erase(void *ctx)
{ (void)ctx; return prepare(signed_session.image_length); }
static bool signed_write(void *ctx, uint32_t offset, const uint8_t *data, size_t n)
{ (void)ctx; if (offset != staged) return false; return native_write(data,n); }
static bool signed_read(void *ctx, uint32_t offset, uint8_t *data, size_t n)
{ (void)ctx; return watchdog_read_ota(offset,data,n); }
bool a1_ota_begin(uint32_t length)
{
    if (!reserve()) return false;
    unsigned_session=false; wire_expected=length; wire_received=0;
    mico_logic_partition_t *part=MicoFlashGetInfo(MICO_PARTITION_OTA_TEMP);
    signed_ota_io io={signed_erase,signed_write,signed_read,NULL};
    if (!part || !signed_ota_begin(&signed_session,length,part->partition_length,3,1,io)) {
        a1_ota_abort("Invalid signed package length"); return false;
    }
    a1_ota_status("authenticating",NULL,0); return true;
}
bool a1_ota_begin_unsigned(uint32_t length)
{
    if (!reserve()) return false;
    unsigned_session=true; wire_expected=length; wire_received=0;
    if (!watchdog_ota_begin()) { a1_ota_abort("Watchdog unavailable"); return false; }
    bool ok = prepare(length);
    watchdog_ota_end();
    return ok;
}
bool a1_ota_write(const uint8_t *data, size_t n)
{
    if (!watchdog_ota_begin()) { a1_ota_abort("Watchdog unavailable"); return false; }
    if (n>wire_expected-wire_received ||
        !(unsigned_session ? native_write(data,n) : signed_ota_write(&signed_session,data,n))) {
        a1_ota_abort("Invalid signature, package or Flash write"); return false;
    }
    if(!unsigned_session && signed_session.authenticated) {
        const uint8_t *h=signed_session.envelope;
        uint64_t counter=(uint64_t)le32(h+24)|((uint64_t)le32(h+28)<<32);
        mico_rtos_lock_mutex(&a1_lock);
        a1_state.ota_signature_verified=true;a1_state.ota_key_id=le32(h+12);
        a1_state.ota_image_length=le32(h+20);
        ota_counter_decimal(counter,a1_state.ota_release_counter);
        memcpy(a1_state.ota_version,h+64,32);
        mico_rtos_unlock_mutex(&a1_lock);
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

bool a1_ota_finish(void)
{
    if (!watchdog_ota_begin()) { a1_ota_abort("Watchdog unavailable"); return false; }
    const char *error = NULL;
    if (!unsigned_session && !signed_ota_finish(&signed_session)) {
        a1_ota_abort("Signed image readback SHA-256 mismatch or incomplete package"); return false;
    }
    mico_rtos_lock_mutex(&a1_lock);a1_state.ota_image_verified=!unsigned_session;
    mico_rtos_unlock_mutex(&a1_lock);
    uint8_t header[36], digest[16], tail[16], data[512];
    a1_ota_status("validating", NULL, 100);
    if (staged != expected) { error = "Truncated OTA body"; goto fail; }
    if (!read_at(A1_APP_OFFSET, header, sizeof(header))) { error = "OTA header read failed"; goto fail; }
    uint32_t payload = le32(header), pid = le32(header + 28);
    if (payload < 120 || payload > A1_APP_LIMIT - 8 ||
        expected != A1_APP_OFFSET + 8 + payload + 16 || le32(header + 8) != 0xc89346 ||
        le32(header + 16) != 3 || le16(header + 4) != le16(header + 6)) {
        error = "Invalid application header or ABI"; goto fail;
    }
    if (pid < A1_APP_CPU_ADDRESS + 8 || pid - A1_APP_CPU_ADDRESS > 8 + payload - sizeof(A1_PRODUCT)) {
        error = "Product pointer outside application"; goto fail;
    }
    char product[sizeof(A1_PRODUCT)];
    if (!read_at(A1_APP_OFFSET + pid - A1_APP_CPU_ADDRESS, product, sizeof(product)) ||
        memcmp(product, A1_PRODUCT, sizeof(product))) { error = "Not an A1 EMW3080 image"; goto fail; }
    uint16_t crc = 0;
    uint32_t position = A1_APP_OFFSET + 8, end = expected - 16;
    while (position < end) {
        unsigned n = end - position > sizeof(data) ? sizeof(data) : end - position;
        if (!read_at(position, data, n)) { error = "OTA flash read failed"; goto fail; }
        crc = a1_crc16(crc, data, n); position += n;
    }
    if (crc != le16(header + 4)) { error = "Application CRC mismatch"; goto fail; }
    if (!md5_range(0, A1_APP_OFFSET, digest) || memcmp(digest, kernel_md5, 16)) {
        error = "Incompatible MiCO kernel"; goto fail;
    }
    if (!md5_range(0, expected - 16, digest) || !read_at(expected - 16, tail, 16) || memcmp(digest, tail, 16)) {
        error = "OTA MD5 mismatch"; goto fail;
    }
    if (mico_ota_switch_to_new_fw(expected, transport_crc) != kNoErr) {
        error = "Failed to persist OTA boot metadata"; goto fail;
    }
    mico_rtos_lock_mutex(&a1_lock);
    strcpy(a1_state.ota_state, "ready"); a1_state.ota_active = false;
    a1_state.reboot_pending = true; a1_state.reboot_at = mico_rtos_get_time() + 1500;
    mico_rtos_unlock_mutex(&a1_lock);
    watchdog_ota_end();
    return true;
fail:
    a1_ota_abort(error); return false;
}

static void download_worker(mico_thread_arg_t arg)
{
    char *url = (char *)arg;
    int fd = -1; const char *error = "HTTP download failed";
    char host[128], path[384], line[640]; unsigned port = 80;
    /* Direct HTTP URLs with Content-Length; no implicit redirect or TLS bypass. */
    const char *authority = url + 7, *slash = strchr(authority, '/');
    size_t authority_len = slash ? (size_t)(slash - authority) : strlen(authority);
    if (!authority_len || authority_len >= sizeof(host)) goto fail;
    memcpy(host, authority, authority_len); host[authority_len] = 0;
    if (slash && strlen(slash) >= sizeof(path)) { error = "URL path too long"; goto fail; }
    snprintf(path, sizeof(path), "%s", slash ? slash : "/");
    char *colon = strchr(host, ':');
    if (colon) {
        char *last; unsigned long value = strtoul(colon + 1, &last, 10);
        if (!value || value > 65535 || *last) goto fail;
        port = value; *colon = 0;
    }
    struct hostent *resolved = gethostbyname(host);
    if (!resolved || !resolved->h_addr_list[0]) { error = "DNS lookup failed"; goto fail; }
    struct sockaddr_in address = {0}; address.sin_family = AF_INET; address.sin_port = htons(port);
    memcpy(&address.sin_addr, resolved->h_addr_list[0], 4);
    fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0 || connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) goto fail;
    snprintf(line, sizeof(line), "GET %s HTTP/1.1\r\nHost: %s:%u\r\nConnection: close\r\nAccept-Encoding: identity\r\n\r\n", path, host, port);
    if (!a1_socket_send(fd, line, strlen(line)) || a1_socket_line(fd, line, sizeof(line)) < 0 ||
        (strncmp(line, "HTTP/1.1 200 ", 13) && strncmp(line, "HTTP/1.0 200 ", 13))) {
        error = "Download requires a direct HTTP 200 response"; goto fail;
    }
    uint32_t length = 0; bool have_length = false; unsigned headers = 0;
    for (;;) {
        int n = a1_socket_line(fd, line, sizeof(line));
        if (n < 0 || (headers += n) > 4096) goto fail;
        if (!n) break;
        if (!strncasecmp(line, "Content-Length:", 15)) {
            char *end; unsigned long parsed = strtoul(line + 15, &end, 10);
            while (*end == ' ') end++;
            if (have_length || *end || parsed > A1_APP_OFFSET + A1_APP_LIMIT + 16 + OTA_ENVELOPE_SIZE) goto fail;
            length = parsed; have_length = true;
        }
        if (!strncasecmp(line, "Transfer-Encoding:", 18) ||
            (!strncasecmp(line, "Content-Encoding:", 17) && strcmp(line + 18, "identity"))) {
            error = "Chunked or compressed downloads are unsupported"; goto fail;
        }
    }
    if (!have_length) { error = "Download is missing Content-Length"; goto fail; }
    unsigned_session=false; wire_expected=length; wire_received=0;
    mico_logic_partition_t *part=MicoFlashGetInfo(MICO_PARTITION_OTA_TEMP);
    signed_ota_io io={signed_erase,signed_write,signed_read,NULL};
    if (!part || !signed_ota_begin(&signed_session,length,part->partition_length,3,1,io)) {
        error="Invalid signed package length"; goto fail;
    }
    uint8_t data[512]; uint32_t deadline = mico_rtos_get_time() + 300000;
    while (wire_received < wire_expected) {
        if ((int32_t)(mico_rtos_get_time() - deadline) >= 0) { error = "Download timed out"; goto fail; }
        unsigned want = wire_expected - wire_received > sizeof(data) ? sizeof(data) : wire_expected - wire_received;
        int n = a1_socket_read(fd, data, want, 15000);
        if (n <= 0) { error = "Download interrupted"; goto fail; }
        if (!a1_ota_write(data, n)) goto done;
    }
    a1_ota_finish(); goto done;
fail:
    a1_ota_abort(error);
done:
    if (fd >= 0) close(fd);
    free(url); mico_rtos_delete_thread(NULL);
}

bool a1_ota_download(const char *url)
{
    if (!url || strncmp(url, "http://", 7) || strlen(url) >= 500 ||
        strpbrk(url, "\r\n @#") || !reserve()) return false;
    char *copy = malloc(strlen(url) + 1);
    if (!copy) { a1_ota_abort("No memory for download"); return false; }
    strcpy(copy, url); a1_ota_status("connecting", NULL, 0);
    if (mico_rtos_create_thread(NULL, MICO_APPLICATION_PRIORITY, "a1_ota", download_worker, 6144,
                                (mico_thread_arg_t)copy) != kNoErr) {
        free(copy); a1_ota_abort("Cannot start OTA worker"); return false;
    }
    return true;
}
