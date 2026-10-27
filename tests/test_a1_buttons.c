#include "../A1/buttons.h"
#include "../A1/filter.h"
#include <assert.h>
#include <stdio.h>

static a1_buttons_t buttons;
static unsigned counts[3], holds[3], recoveries;
static unsigned network_resets, full_resets;
static void sample(bool valid, bool power, bool wifi, uint32_t at)
{
    key_event_t out[3];
    a1_buttons_sample(&buttons, valid, power, wifi, at, out);
    for (unsigned i = 0; i < 3; i++) {
        if (out[i].type == KEY_EVENT_CLICKS) counts[i] += out[i].count;
        if (out[i].type == KEY_EVENT_HOLD) holds[i] = out[i].count;
        if (out[i].type >= KEY_EVENT_NETWORK_RESET) recoveries++;
        if (out[i].type == KEY_EVENT_NETWORK_RESET) network_resets++;
        if (out[i].type == KEY_EVENT_FULL_RESET) full_resets++;
    }
}

static void clear_buttons(void)
{
    memset(&buttons, 0, sizeof(buttons));
    memset(counts, 0, sizeof(counts));
    memset(holds, 0, sizeof(holds));
    recoveries = network_resets = full_resets = 0;
}

static void wifi_tap(uint32_t *at)
{
    for (unsigned i = 0; i < 6; i++, *at += 40)
        sample(true, false, i < 3, *at);
}

static void wifi_hold(uint32_t *at, unsigned duration)
{
    for (unsigned i = 0; i < duration; i += 40, *at += 40) {
        sample(true, false, true, *at);
        assert(!recoveries); /* Recovery is never executed while reported held. */
    }
    sample(true, false, false, *at); *at += 40;
    sample(true, false, false, *at); *at += 40;
}

static void short_panel_recovery(void)
{
    /* The live panel returns released after ~2-3 s even with a finger held.
     * Exercise the actual arbitration with a deliberate click/hold sequence
     * that completes before that limit, without pretending zero means held. */
    for (unsigned taps = 2; taps <= 3; taps++) {
        clear_buttons(); uint32_t at = 100000;
        for (unsigned i = 0; i < taps; i++) wifi_tap(&at);
        wifi_hold(&at, 1600);
        assert(recoveries == 1);
        assert(network_resets == (taps == 2));
        assert(full_resets == (taps == 3));
        for (unsigned i = 0; i < 20; i++, at += 40)
            sample(true, false, false, at);
        assert(recoveries == 1 && !counts[1] && !holds[1]);
    }
    /* Ordinary single holds and other click prefixes retain their mappings. */
    for (unsigned taps = 0; taps <= 4; taps++) {
        if (taps == 2 || taps == 3) continue;
        clear_buttons(); uint32_t at = 100000;
        for (unsigned i = 0; i < taps; i++) wifi_tap(&at);
        wifi_hold(&at, 1600);
        assert(!recoveries && holds[1] == 1);
    }
    /* Completed click groups, short final presses and key bounce cannot reset. */
    clear_buttons(); uint32_t at = 100000;
    wifi_tap(&at); wifi_tap(&at);
    for (unsigned i = 0; i < 10; i++, at += 40)
        sample(true, false, false, at);
    assert(counts[1] == 2);
    wifi_hold(&at, 1600);
    assert(!recoveries && holds[1] == 1);
    /* The click deadline may fall between the last idle scan and the next
     * press. That expired group must not arm a recovery on the new hold. */
    clear_buttons(); at = 100000;
    wifi_tap(&at); wifi_tap(&at);
    for (unsigned i = 0; i < 5; i++, at += 40)
        sample(true, false, false, at);
    assert(!counts[1]);
    wifi_hold(&at, 1600);
    assert(!recoveries && counts[1] == 2 && holds[1] == 1);
    /* The same boundary between prefix taps cannot combine old and new taps. */
    clear_buttons(); at = 100000;
    wifi_tap(&at);
    for (unsigned i = 0; i < 5; i++, at += 40)
        sample(true, false, false, at);
    wifi_tap(&at); wifi_hold(&at, 1600);
    assert(!recoveries && counts[1] == 1 && holds[1] == 1);
    clear_buttons(); at = 100000;
    wifi_tap(&at); wifi_tap(&at); wifi_hold(&at, 800);
    assert(!recoveries);
    clear_buttons(); at = 100000;
    for (unsigned i = 0; i < 3; i++, at += 80) {
        sample(true, false, true, at);
        sample(true, false, false, at + 40);
    }
    wifi_hold(&at, 1600);
    assert(!recoveries && holds[1] == 1);
    /* A failed read invalidates the prefix and the hold until release. */
    clear_buttons(); at = 100000;
    wifi_tap(&at); wifi_tap(&at); wifi_tap(&at);
    sample(true, false, true, at); at += 40;
    sample(true, false, true, at); at += 40;
    sample(false, false, false, at); at += 40;
    wifi_hold(&at, 1600);
    assert(!recoveries && !holds[1]);
    /* A chord cancels the prefix, including staggered key release. */
    clear_buttons(); at = 100000;
    wifi_tap(&at); wifi_tap(&at); wifi_tap(&at);
    for (unsigned i = 0; i < 40; i++, at += 40)
        sample(true, true, true, at);
    sample(true, false, true, at); at += 40;
    sample(true, false, true, at); at += 40;
    sample(true, false, false, at); at += 40;
    sample(true, false, false, at); at += 40;
    sample(true, false, false, at + 400);
    assert(!recoveries && !counts[1] && !holds[1]);
    /* Unsigned RTOS time wrap does not alter the deliberate recovery. */
    clear_buttons(); at = UINT32_MAX - 600;
    wifi_tap(&at); wifi_tap(&at); wifi_tap(&at);
    wifi_hold(&at, 1600);
    assert(full_resets == 1 && network_resets == 0);
}
int main(void)
{
    sample(true, true, false, 0); sample(true, true, false, 40);
    sample(true, true, true, 80); sample(true, true, true, 120);
    sample(true, false, true, 400); sample(true, false, true, 440);
    sample(true, false, true, 800);
    sample(true, false, false, 840); sample(true, false, false, 880);
    sample(true, false, false, 920);
    assert(counts[2] == 1 && !counts[0] && !counts[1]);
    /* Subsequent singles work after a staggered chord release. */
    sample(true, false, true, 1000); sample(true, false, true, 1040);
    sample(true, false, false, 1100); sample(true, false, false, 1140);
    sample(true, false, false, 1400); assert(counts[1] == 1);
    /* A chord held over five seconds never becomes Wi-Fi recovery. */
    sample(true, true, true, 2000); sample(true, true, true, 2040);
    sample(true, true, true, 8000); sample(true, false, true, 8040);
    sample(true, false, true, 8080); sample(true, false, false, 8100);
    sample(true, false, false, 8140); sample(true, false, false, 8500);
    assert(!recoveries && counts[1] == 1 && counts[2] == 1);
    sample(true, true, true, 9000); sample(true, true, true, 9040);
    sample(true, false, false, 11400); sample(true, false, false, 11440);
    sample(true, false, false, 11800); assert(holds[2] == 2);
    /* Failed reads cancel both a pending chord and its release. */
    sample(true, true, true, 12000); sample(true, true, true, 12040);
    sample(false, false, false, 12100);
    sample(true, false, false, 12200); sample(true, false, false, 12240);
    sample(true, false, false, 12600); assert(counts[2] == 1);
    sample(true, false, true, 13000); sample(true, false, true, 13040);
    sample(true, false, false, 18000); sample(true, false, false, 18040);
    assert(recoveries == 1);

    memset(&buttons, 0, sizeof(buttons));
    memset(counts, 0, sizeof(counts));
    /* Wi-Fi first, then a double chord; neither single key fires. */
    sample(true, false, true, 19000); sample(true, false, true, 19040);
    sample(true, true, true, 19080); sample(true, true, true, 19120);
    sample(true, false, false, 19160); sample(true, false, false, 19200);
    sample(true, true, true, 19240); sample(true, true, true, 19280);
    sample(true, false, false, 19320); sample(true, false, false, 19360);
    sample(true, false, false, 19700);
    assert(counts[2] == 2 && !counts[0] && !counts[1] && recoveries == 1);

    short_panel_recovery();
    puts("A1 chord arbitration, staggered release, recovery isolation and short-panel recovery passed");
}
