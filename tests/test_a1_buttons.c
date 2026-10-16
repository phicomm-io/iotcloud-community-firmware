#include "../A1/buttons.h"
#include "../A1/filter.h"
#include <assert.h>
#include <stdio.h>

static a1_buttons_t buttons;
static unsigned counts[3], holds[3], recoveries;
static void sample(bool valid, bool power, bool wifi, uint32_t at)
{
    key_event_t out[3];
    a1_buttons_sample(&buttons, valid, power, wifi, at, out);
    for (unsigned i = 0; i < 3; i++) {
        if (out[i].type == KEY_EVENT_CLICKS) counts[i] += out[i].count;
        if (out[i].type == KEY_EVENT_HOLD) holds[i] = out[i].count;
        if (out[i].type >= KEY_EVENT_NETWORK_RESET) recoveries++;
    }
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

    puts("A1 chord arbitration, staggered release, recovery isolation passed");
}
