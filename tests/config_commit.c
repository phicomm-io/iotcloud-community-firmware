/* Execute extracted, pinned SDK storage/CRC functions against faultable Flash. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>

typedef int OSStatus;
typedef unsigned mico_mutex_t;
typedef enum { MICO_PARTITION_PARAMETER_1, MICO_PARTITION_PARAMETER_2 } mico_partition_t;
typedef struct { uint32_t partition_length; } mico_logic_partition_t;
typedef struct { uint16_t crc; } CRC16_Context;
typedef struct {
    uint32_t magic_number;
    int32_t seed, dhcpEnable;
    char localIp[16], netMask[16], gateWay[16], dnsServer[16];
} mico_config_t;
typedef struct { uint8_t bootTable[24]; mico_config_t micoSystemConfig; } system_config_t;
typedef system_config_t mico_Context_t;
typedef struct {
    system_config_t flashContentInRam;
    uint8_t *user_config_data;
    uint32_t user_config_data_size;
    uint8_t extra_ap[32];
    struct { char localIp[16], netMask[16], gateWay[16], dnsServer[16]; } micoStatus;
} system_context_t;

#define kNoErr 0
#define kNotPreparedErr -1
#define kWriteErr -2
#define kReadErr -3
#define kSizeErr -4
#define kNoMemoryErr -5
#define kParamErr -6
#define FAULT -77
#define LEGACY -88
#define CRC_SIZE 2
#define SYS_MAGIC_NUMBR 0xa43e2165u
#define DHCP_Disable 0
#define PARA_MICO_DATA_SECTION 0
#define PARA_APP_DATA_SECTION 1
#define PARA_SYS_EXTRA_SECTION 2
#define require_action(c,l,a) do { if (!(c)) { a; goto l; } } while (0)
#define require_noerr(e,l) do { if ((e) != 0) goto l; } while (0)
#define para_log(...) ((void)0)

enum { FLASH_SIZE = 8192, APP_SIZE = 2928, EXTRA_OFFSET = 6144, MAX_OPS = 256 };
typedef struct { char kind; unsigned part, offset, length; } operation_t;
static uint8_t flash[2][FLASH_SIZE], app[APP_SIZE];
static mico_logic_partition_t partitions[2] = {{FLASH_SIZE}, {FLASH_SIZE}};
static system_context_t context, *sys_context = &context;
static const uint32_t mico_context_section_offsets[] = {24, sizeof(system_config_t), EXTRA_OFFSET};
static int32_t seedNum;
static mico_mutex_t para_flash_mutex;
static bool persist_allowed = true;
static unsigned indeterminate_hooks;
static operation_t operations[MAX_OPS];
static unsigned operation_count, mutations, legacy_calls, restore_calls;
static unsigned fault_at, fault_mode, allocation_count, allocation_fail_at, live_bytes, peak_bytes;
static unsigned power_at;
static bool power_after, persistent_reads;
static jmp_buf power_jump;
enum { BEFORE = 1, AFTER, DROP, CORRUPT, SHORT, PERSISTENT };

static void *test_malloc(size_t size)
{
    allocation_count++;
    if (allocation_count == allocation_fail_at) return NULL;
    assert(size == 1024);
    void *p = malloc(size);
    assert(p);
    live_bytes += (unsigned)size;
    if (live_bytes > peak_bytes) peak_bytes = live_bytes;
    return p;
}
static void test_free(void *p)
{
    if (p) { assert(live_bytes >= 1024); live_bytes -= 1024; }
    free(p);
}
static bool hit(char kind, unsigned part, unsigned offset, unsigned length)
{
    assert(operation_count < MAX_OPS);
    operations[operation_count++] = (operation_t){kind, part, offset, length};
    if (power_at == operation_count && !power_after) longjmp(power_jump, 1);
    if (fault_at == operation_count && fault_mode == PERSISTENT) persistent_reads = true;
    return fault_at == operation_count || (persistent_reads && kind == 'R');
}
static void completed(void)
{
    if (power_at == operation_count && power_after) longjmp(power_jump, 1);
}
static mico_logic_partition_t *MicoFlashGetInfo(mico_partition_t p)
{
    assert(p == 0 || p == 1);
    return &partitions[p];
}
static OSStatus MicoFlashErase(mico_partition_t p, uint32_t offset, uint32_t length)
{
    assert(offset + length <= FLASH_SIZE);
    bool fail = hit('E', p, offset, length);
    if (fail && (fault_mode == BEFORE || fault_mode == PERSISTENT)) return FAULT;
    mutations++;
    if (!(fail && fault_mode == DROP))
        memset(flash[p] + offset, 0xff, fail && fault_mode == SHORT ? length / 2 : length);
    completed();
    return fail && fault_mode == AFTER ? FAULT : kNoErr;
}
static OSStatus MicoFlashWrite(mico_partition_t p, uint32_t *offset, uint8_t *data, uint32_t length)
{
    assert(*offset + length <= FLASH_SIZE);
    bool fail = hit('W', p, *offset, length);
    if (fail && (fault_mode == BEFORE || fault_mode == PERSISTENT)) return FAULT;
    unsigned start = *offset, n = fail && fault_mode == SHORT ? length / 2 : length;
    mutations++;
    if (!(fail && fault_mode == DROP)) {
        for (unsigned i = 0; i < n; ++i) flash[p][start + i] &= data[i];
        if (fail && fault_mode == CORRUPT && n) flash[p][start] ^= 1;
    }
    *offset += n;
    completed();
    return fail && fault_mode == AFTER ? FAULT : kNoErr;
}
static OSStatus MicoFlashRead(mico_partition_t p, uint32_t *offset, uint8_t *data, uint32_t length)
{
    assert(*offset + length <= FLASH_SIZE);
    bool fail = hit('R', p, *offset, length);
    if (fail && (fault_mode == BEFORE || fault_mode == PERSISTENT)) return FAULT;
    unsigned n = fail && fault_mode == SHORT ? length / 2 : length;
    memcpy(data, flash[p] + *offset, n);
    if (fail && fault_mode == CORRUPT && n) data[0] ^= 1;
    *offset += n;
    completed();
    return fail && fault_mode == AFTER ? FAULT : kNoErr;
}
static void mico_rtos_lock_mutex(mico_mutex_t *mutex) { assert(!*mutex); *mutex = 1; }
static void mico_rtos_unlock_mutex(mico_mutex_t *mutex) { assert(*mutex); *mutex = 0; }
static mico_Context_t *mico_system_context_get(void) { return &context.flashContentInRam; }
bool app_config_persist_allowed(void) { return persist_allowed; }
void app_config_commit_indeterminate(void) { indeterminate_hooks++; }
static OSStatus try_old_para(system_context_t *p) { (void)p; legacy_calls++; return LEGACY; }
static OSStatus mico_system_context_restore(mico_Context_t *p)
{
    restore_calls++;
    memset(p, 0, sizeof(*p));
    p->micoSystemConfig.magic_number = SYS_MAGIC_NUMBR;
    return kNoErr;
}

#define malloc test_malloc
#define free test_free
/* SDK_FUNCTIONS */
#undef malloc
#undef free

static uint32_t revision(void)
{
    uint32_t n;
    memcpy(&n, app + 2856, sizeof(n));
    return n;
}
static void set_revision(uint32_t n) { memcpy(app + 2856, &n, sizeof(n)); }
static void reset_io(void)
{
    assert(!live_bytes);
    operation_count = mutations = legacy_calls = restore_calls = 0;
    fault_at = fault_mode = allocation_count = allocation_fail_at = peak_bytes = 0;
    power_at = 0; power_after = persistent_reads = false;
    para_flash_mutex = 0;
}
static void setup(void)
{
    memset(flash, 0xff, sizeof(flash));
    memset(&context, 0, sizeof(context));
    memset(app, 0x42, sizeof(app));
    memset(context.extra_ap, 0x31, sizeof(context.extra_ap));
    context.user_config_data = app;
    context.user_config_data_size = sizeof(app);
    context.flashContentInRam.micoSystemConfig.magic_number = SYS_MAGIC_NUMBR;
    context.flashContentInRam.micoSystemConfig.dhcpEnable = 1;
    partitions[0].partition_length = partitions[1].partition_length = FLASH_SIZE;
    persist_allowed = true;
    para_commit_uncertain = false;
    indeterminate_hooks = 0;
    seedNum = 7;
    reset_io();
    set_revision(7);
    assert(mico_system_context_update(mico_system_context_get()) == kNoErr);
    assert(!memcmp(flash[0], flash[1], sizeof(flash[0])));
    assert(peak_bytes <= 1024);
    reset_io();
}
static bool raw_valid(unsigned p)
{
    CRC16_Context crc;
    uint16_t actual, stored;
    CRC16_Init(&crc);
    CRC16_Update(&crc, flash[p] + 24, FLASH_SIZE - CRC_SIZE - 24);
    CRC16_Final(&crc, &actual);
    memcpy(&stored, flash[p] + FLASH_SIZE - CRC_SIZE, sizeof(stored));
    return actual == stored;
}
static uint32_t reboot(void)
{
    reset_io();
    memset(app, 0, sizeof(app));
    memset(&context.flashContentInRam, 0, sizeof(context.flashContentInRam));
    assert(MICOReadConfiguration(&context) == kNoErr);
    assert(!restore_calls && !legacy_calls && !para_flash_mutex && !live_bytes);
    return revision();
}
static unsigned normal_operations(operation_t *saved)
{
    setup(); set_revision(8);
    assert(mico_system_context_update(mico_system_context_get()) == kNoErr);
    unsigned n = operation_count;
    memcpy(saved, operations, n * sizeof(*saved));
    assert(raw_valid(0) && raw_valid(1) && peak_bytes <= 1024);
    return n;
}
static void transaction_faults(void)
{
    operation_t normal[MAX_OPS];
    unsigned count = normal_operations(normal), scenarios = 0;
    for (unsigned mode = BEFORE; mode <= AFTER; ++mode) for (unsigned step = 1; step <= count; ++step) {
        setup(); set_revision(8); fault_at = step; fault_mode = mode;
        OSStatus status = mico_system_context_update(mico_system_context_get());
        assert(!para_flash_mutex && !live_bytes && peak_bytes <= 1024);
        assert(!indeterminate_hooks && !para_commit_uncertain);
        if (status) set_revision(7); /* Actual application caller's rollback rule. */
        uint32_t ram = revision();
        assert(raw_valid(0) || raw_valid(1));
        assert(reboot() == ram);
        scenarios++;
    }
    /* Silent write failures are important: actual MOC discards write status. */
    for (unsigned mode = DROP; mode <= SHORT; ++mode) for (unsigned step = 1; step <= count; ++step) {
        operation_t op = normal[step - 1];
        if (op.kind != 'W' && op.kind != 'E') continue;
        setup(); set_revision(8); fault_at = step; fault_mode = mode;
        OSStatus status = mico_system_context_update(mico_system_context_get());
        assert(!indeterminate_hooks && !para_commit_uncertain);
        if (status) set_revision(7);
        uint32_t ram = revision();
        assert(raw_valid(0) || raw_valid(1));
        assert(reboot() == ram);
        scenarios++;
    }
    printf("actual SDK transaction faults: operations=%u before/after/silent/partial=%u RAM/boot divergences=0\n", count, scenarios);
}
static void powerloss_at(unsigned step, bool after, unsigned tail)
{
    setup(); set_revision(8); power_at = step; power_after = after;
    if (setjmp(power_jump) == 0) {
        (void)mico_system_context_update(mico_system_context_get());
        assert(!"missing power loss injection");
    }
    /* Power loss discards allocations/mutex/RAM, not Flash. */
    assert(raw_valid(0) || raw_valid(1));
    assert(live_bytes == 0 || live_bytes == 1024);
    live_bytes = 0;
    assert(reboot() == (step > tail || (step == tail && after) ? 8u : 7u));
}
static void powerloss(void)
{
    operation_t normal[MAX_OPS];
    unsigned count = normal_operations(normal), tail = 0;
    for (unsigned i = 0; i < count; ++i)
        if (normal[i].kind == 'W' && normal[i].part == 0 && normal[i].offset == FLASH_SIZE - CRC_SIZE) tail = i + 1;
    assert(tail);
    for (unsigned after = 0; after < 2; ++after) for (unsigned step = 1; step <= count; ++step) {
        powerloss_at(step, after, tail);
    }
    printf("actual SDK power loss: %u boundaries before/after last CRC commit PASS\n", count * 2);
}
static void degraded_retry(void)
{
    operation_t normal[MAX_OPS];
    unsigned count = normal_operations(normal), backup_erase = 0;
    for (unsigned i = 0; i < count; ++i)
        if (normal[i].kind == 'E' && normal[i].part == 1) backup_erase = i + 1;
    assert(backup_erase);
    setup(); set_revision(8); fault_at = backup_erase; fault_mode = AFTER;
    assert(mico_system_context_update(mico_system_context_get()) == kNoErr);
    assert(raw_valid(0) && !raw_valid(1));
    uint8_t degraded[2][FLASH_SIZE];
    memcpy(degraded, flash, sizeof(flash));
    reset_io(); set_revision(9);
    assert(mico_system_context_update(mico_system_context_get()) == kNoErr);
    unsigned n = operation_count;
    for (unsigned mode = BEFORE; mode <= AFTER; ++mode) for (unsigned step = 1; step <= n; ++step) {
        reset_io(); memcpy(flash, degraded, sizeof(flash)); set_revision(9);
        fault_at = step; fault_mode = mode;
        OSStatus status = mico_system_context_update(mico_system_context_get());
        if (status) set_revision(8);
        uint32_t ram = revision();
        assert(raw_valid(0) || raw_valid(1));
        assert(reboot() == ram);
    }
    printf("actual SDK degraded redundancy retry: %u fault cases preserve last accepted revision\n", n * 2);
}
static void boot_faults(void)
{
    for (unsigned damaged = 0; damaged < 3; ++damaged) {
        setup();
        if (damaged < 2) flash[damaged][200] ^= 1;
        uint8_t saved[2][FLASH_SIZE]; memcpy(saved, flash, sizeof(flash));
        reset_io(); assert(MICOReadConfiguration(&context) == kNoErr);
        operation_t normal[MAX_OPS]; unsigned n = operation_count;
        memcpy(normal, operations, n * sizeof(*normal));
        unsigned first_mutation = n + 1;
        for (unsigned i = 0; i < n; ++i)
            if (normal[i].kind != 'R') { first_mutation = i + 1; break; }
        assert(revision() == 7);
        for (unsigned mode = BEFORE; mode <= AFTER; ++mode) for (unsigned step = 1; step <= n; ++step) {
            reset_io(); memcpy(flash, saved, sizeof(flash)); memset(app, 0, sizeof(app));
            para_commit_uncertain = true;
            fault_at = step; fault_mode = mode;
            OSStatus status = MICOReadConfiguration(&context);
            assert(status == FAULT && para_commit_uncertain);
            assert(raw_valid(0) || raw_valid(1));
            /* Initial verification reads must not trigger destructive repair. */
            if (step < first_mutation) assert(!mutations);
            assert(!live_bytes && !para_flash_mutex);
            assert(reboot() == 7);
        }
        (void)normal;
    }
    puts("actual SDK boot selection/repair: full read errors propagate, valid source retained PASS");
}
static void recovery_and_boundaries(void)
{
    setup(); persist_allowed = false; set_revision(8);
    assert(mico_system_context_update(mico_system_context_get()) == kNoErr && !operation_count);
    flash[0][200] ^= 1;
    assert(MICOReadConfiguration(&context) == kNoErr && revision() == 7 && !mutations);
    assert(!raw_valid(0) && raw_valid(1));
    reset_io(); assert(internal_update_config(NULL) == kNotPreparedErr && !operation_count && !para_flash_mutex);
    setup(); context.user_config_data_size = FLASH_SIZE;
    assert(internal_update_config(&context) == kSizeErr && !operation_count && !mutations);
    setup();
    /* Legal CRC zero must be accepted, unlike read failure. */
    uint8_t *suffix = flash[0] + FLASH_SIZE - CRC_SIZE - 2;
    bool found_zero = false;
    CRC16_Context prefix;
    CRC16_Init(&prefix);
    CRC16_Update(&prefix, flash[0] + 24, FLASH_SIZE - CRC_SIZE - 24 - 2);
    for (unsigned i = 0; i <= UINT16_MAX; ++i) {
        memcpy(suffix, &i, 2);
        CRC16_Context crc = prefix; uint16_t result;
        CRC16_Update(&crc, suffix, 2); CRC16_Final(&crc, &result);
        if (!result) { memset(flash[0] + FLASH_SIZE - CRC_SIZE, 0, CRC_SIZE); found_zero = true; break; }
    }
    assert(found_zero);
    uint16_t crc; bool valid;
    assert(para_valid(0, &valid, &crc) == kNoErr && valid && crc == 0);
    assert(para_copy(1, 0) == kNoErr && raw_valid(1));
    reset_io(); fault_at = 1; fault_mode = BEFORE;
    assert(para_valid(0, &valid, &crc) == FAULT && !valid && !mutations);
    reset_io(); fault_at = 1; fault_mode = SHORT;
    assert(para_valid(0, &valid, &crc) == kReadErr && !valid && !mutations);
    setup(); memset(flash, 0xff, sizeof(flash));
    assert(MICOReadConfiguration(&context) == LEGACY && legacy_calls == 1 && !mutations);
    puts("actual SDK recovery/invalid layout/null/CRC0/legacy fallback PASS");
}
static void allocation_faults(void)
{
    setup(); set_revision(8); assert(mico_system_context_update(mico_system_context_get()) == kNoErr);
    unsigned n = allocation_count;
    for (unsigned i = 1; i <= n; ++i) {
        setup(); set_revision(8); allocation_fail_at = i;
        OSStatus status = mico_system_context_update(mico_system_context_get());
        if (status) set_revision(7);
        uint32_t ram = revision();
        assert(!live_bytes && !para_flash_mutex && peak_bytes <= 1024);
        assert(reboot() == ram);
    }
    printf("actual SDK allocation failures: %u, peak scratch=1024 bytes PASS\n", n);
}
/* Adjust an otherwise ordinary application's final two bytes to a given CRC. */
static void app_choose_crc(uint16_t target)
{
    uint8_t scratch[1024];
    uint16_t influence[16], base;
    app[APP_SIZE - 2] = app[APP_SIZE - 1] = 0;
    base = para_config_crc(&context, 0, scratch);
    for (unsigned bit = 0; bit < 16; ++bit) {
        app[APP_SIZE - 2 + bit / 8] = (uint8_t)(1u << (bit % 8));
        influence[bit] = para_config_crc(&context, 0, scratch) ^ base;
        app[APP_SIZE - 2 + bit / 8] = 0;
    }
    bool found = false;
    for (unsigned candidate = 0; candidate <= UINT16_MAX; ++candidate) {
        uint16_t crc = base;
        for (unsigned bit = 0; bit < 16; ++bit) if (candidate & (1u << bit)) crc ^= influence[bit];
        if (crc == target) {
            app[APP_SIZE - 2] = (uint8_t)candidate;
            app[APP_SIZE - 1] = (uint8_t)(candidate >> 8);
            found = true; break;
        }
    }
    assert(found && para_config_crc(&context, 0, scratch) == target);
}
static void erased_crc_is_not_a_commit(void)
{
    setup(); set_revision(8);
    uint32_t seed = (uint32_t)context.flashContentInRam.micoSystemConfig.seed;
    app_choose_crc(UINT16_MAX);
    assert(internal_update_config(&context) == kNoErr);
    assert((uint32_t)context.flashContentInRam.micoSystemConfig.seed == (seed ^ 1u));
    uint16_t tail; memcpy(&tail, flash[0] + FLASH_SIZE - CRC_SIZE, sizeof(tail));
    assert(tail != UINT16_MAX && raw_valid(0) && raw_valid(1) && reboot() == 8);
    puts("actual SDK CRC-FFFF payload: SDK seed bit changes before erase, erased tail cannot commit PASS");
}
static void crc_equal_different_contents(void)
{
    setup();
    uint16_t previous_crc;
    memcpy(&previous_crc, flash[1] + FLASH_SIZE - CRC_SIZE, sizeof(previous_crc));
    assert(previous_crc != UINT16_MAX);
    set_revision(8);
    app_choose_crc(previous_crc);
    uint8_t scratch[1024];
    assert(para_write_config(0, &context, scratch) == kNoErr);
    assert(raw_valid(0) && raw_valid(1));
    assert(!memcmp(flash[0] + FLASH_SIZE - CRC_SIZE, flash[1] + FLASH_SIZE - CRC_SIZE, CRC_SIZE));
    assert(memcmp(flash[0] + 24, flash[1] + 24, FLASH_SIZE - 24));
    uint8_t pair[2][FLASH_SIZE], current[APP_SIZE];
    memcpy(pair, flash, sizeof(pair));memcpy(current, app, sizeof(current));
    reset_io();
    assert(MICOReadConfiguration(&context) == kNoErr && revision() == 8);
    assert(!memcmp(flash[0] + 24, flash[1] + 24, FLASH_SIZE - 24));
    assert(raw_valid(0) && raw_valid(1) && peak_bytes <= 1024 && !live_bytes);
    reset_io();memcpy(flash, pair, sizeof(pair));memcpy(app, current, sizeof(app));
    set_revision(9);
    assert(mico_system_context_update(mico_system_context_get()) == kNoErr);
    unsigned primary_erase = 0, backup_erase = 0;
    for (unsigned i = 0; i < operation_count; ++i) {
        if (operations[i].kind != 'E') continue;
        if (!operations[i].part && !primary_erase) primary_erase = i + 1;
        if (operations[i].part && !backup_erase) backup_erase = i + 1;
    }
    assert(backup_erase && primary_erase > backup_erase);
    reset_io();memcpy(flash, pair, sizeof(pair));memcpy(app, current, sizeof(app));
    set_revision(9);fault_at = primary_erase;fault_mode = AFTER;
    assert(mico_system_context_update(mico_system_context_get()) == FAULT);
    memcpy(app, current, sizeof(app)); /* Caller restores the actual old RAM. */
    assert(!raw_valid(0) && raw_valid(1) && peak_bytes <= 1024 && !live_bytes);
    assert(reboot() == 8);
    puts("actual SDK two CRC-valid copies with equal CRC/different data: boot synchronizes; next save preserves current primary before erase; peak scratch=1024 PASS");
}
static void unknown_commit(void)
{
    operation_t normal[MAX_OPS]; unsigned n = normal_operations(normal), tail_read = 0;
    for (unsigned i = 1; i < n; ++i)
        if (normal[i-1].kind == 'W' && normal[i-1].part == 0 && normal[i-1].offset == FLASH_SIZE - CRC_SIZE)
            tail_read = i + 1;
    assert(tail_read);
    setup(); uint8_t backup[FLASH_SIZE]; memcpy(backup, flash[1], sizeof(backup));
    set_revision(8); fault_at = tail_read; fault_mode = PERSISTENT;
    assert(mico_system_context_update(mico_system_context_get()) == FAULT);
    assert(!memcmp(backup, flash[1], sizeof(backup)) && raw_valid(0));
    assert(para_commit_uncertain && indeterminate_hooks == 1);
    assert(!live_bytes && !para_flash_mutex);
    unsigned calls = operation_count;
    set_revision(9);
    assert(mico_system_context_update(mico_system_context_get()) == kNotPreparedErr);
    assert(operation_count == calls && indeterminate_hooks == 1);
    reset_io(); fault_at = 1; fault_mode = BEFORE;
    assert(MICOReadConfiguration(&context) == FAULT && para_commit_uncertain && !mutations);
    /* No claim that a permanent post-commit read failure rolled Flash back. */
    assert(reboot() == 8);
    assert(!para_commit_uncertain);
    reset_io(); set_revision(9);
    assert(mico_system_context_update(mico_system_context_get()) == kNoErr && mutations);
    puts("actual SDK persistent post-commit read failure: INDETERMINATE; hook once, backup untouched, further writes rejected, failed reread stays latched, trusted reread loads new and unlocks (not rollback PASS)");
    unsigned backup_tail_read = 0;
    for (unsigned i = 1; i < n; ++i)
        if (normal[i-1].kind == 'W' && normal[i-1].part == 1 && normal[i-1].offset == FLASH_SIZE - CRC_SIZE)
            backup_tail_read = i + 1;
    assert(backup_tail_read);
    setup(); set_revision(8); fault_at = backup_tail_read; fault_mode = PERSISTENT;
    assert(mico_system_context_update(mico_system_context_get()) == kNoErr);
    assert(!para_commit_uncertain && !indeterminate_hooks && raw_valid(0));
    assert(reboot() == 8);
    puts("actual SDK persistent backup read failure: accepted primary, no indeterminate hook PASS");
}
int main(void)
{
    /* Resets abandon old process heaps; the separate run disables leak-only
     * reporting while retaining address/undefined-behaviour checks. */
    if (getenv("CONFIG_COMMIT_POWERLOSS")) { powerloss(); return 0; }
    transaction_faults();
    degraded_retry();
    boot_faults();
    recovery_and_boundaries();
    allocation_faults();
    erased_crc_is_not_a_commit();
    crc_equal_different_contents();
    unknown_commit();
    return 0;
}
