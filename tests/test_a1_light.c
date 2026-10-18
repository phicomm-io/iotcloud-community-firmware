#include <assert.h>
#include <string.h>
#include "../A1/ring_feedback.h"
#include "../A1/light.h"

int main(void)
{
    uint32_t colors[32];
    const uint32_t color = 0x123456;
    const uint32_t half = 0x091a2b;
    unsigned previous = 0;
    for (unsigned speed = 0; speed <= 100; speed++) {
        a1_fan_frame(colors, speed, color);
        unsigned energy = 0;
        for (unsigned i = 0; i < 31; i++) {
            assert(colors[i] == 0 || colors[i] == half || colors[i] == color);
            energy += colors[i] == color ? 2 : colors[i] == half;
        }
        assert(colors[31] == 0);
        assert(energy >= previous);
        previous = energy;
        for (unsigned d = 1; d <= 15; d++) {
            assert(colors[(11 + 31 - d) % 31] == colors[(11 + d) % 31]);
            if (d > 1 && colors[(11 + d) % 31])
                assert(colors[(11 + d - 1) % 31] == color);
        }
    }
    a1_fan_frame(colors, 0, color);
    for (unsigned i = 0; i < 32; i++) assert(colors[i] == 0);
    a1_fan_frame(colors, 2, color);
    assert(colors[11] == half && colors[10] == 0 && colors[12] == 0);
    a1_fan_frame(colors, 6, color);
    assert(colors[11] == color && colors[10] == half && colors[12] == half);
    a1_fan_frame(colors, 10, color);
    assert(colors[11] == color && colors[10] == color && colors[12] == color);
    a1_fan_frame(colors, 99, color);
    assert(colors[26] == half && colors[27] == half);
    a1_fan_frame(colors, 150, color);
    for (unsigned i = 0; i < 31; i++) assert(colors[i] == color);
    assert(colors[31] == 0);
    a1_fan_frame(colors, 100, 0);
    for (unsigned i = 0; i < 32; i++) assert(colors[i] == 0);
    a1_fan_frame(colors, 2, 0xff0103);
    assert(colors[11] == 0x7f0001);
    for (unsigned slot = 1; slot <= 32; slot++) {
        assert(a1_test_frame(colors, slot, 100, 200, false));
        for (unsigned i = 0; i < 32; i++)
            assert(colors[i] == (i == 0 ? 0x002000u : i + 1 == slot ? 0x202020u : 0));
    }
    colors[0] = color;
    assert(!a1_test_frame(colors, 0, 100, 200, false));
    assert(!a1_test_frame(colors, 33, 100, 200, false));
    assert(!a1_test_frame(colors, 1, 200, 200, false));
    assert(colors[0] == color);
    assert(a1_test_frame(colors, 1, UINT32_MAX - 50, 50, false));
    assert(a1_test_frame(colors, 11, 100, 200, true));
    for (unsigned i = 0; i < 32; i++)
        assert(colors[i] == (i == 10 ? 0x202020u : i == 9 || i == 11 ? 0x200000u : 0));
    a1_key_light_t feedback = {0};
    colors[0] = 0x123456;
    assert(!a1_key_light_frame(&feedback, 0, 64, colors));
    assert(colors[0] == 0x123456);
    const uint32_t expected[] = {0x400000, 0x000040, 0x004000};
    for (unsigned kind = 0; kind < 3; kind++) {
        a1_key_light_start(&feedback, kind == 1, kind == 2, false, false, 1000);
        assert(a1_key_light_frame(&feedback, 1000, 64, colors));
        for (unsigned i = 0; i < 32; i++) assert(colors[i] == expected[kind]);
        assert(a1_key_light_frame(&feedback, 1299, 64, colors));
        assert(!a1_key_light_frame(&feedback, 1300, 64, colors));
        assert(!feedback.active);
    }
    /* Running power taps and hold pulses must not cover mode/airflow frames. */
    for (unsigned hold = 0; hold < 2; hold++) {
        a1_key_light_start(&feedback, 0, false, false, false, 2000);
        a1_ring_feedback(colors, 46, 64, a1_mode_color(false));
        uint32_t before[32]; memcpy(before, colors, sizeof(before));
        a1_key_light_start(&feedback, 0, false, true, hold, 2001);
        assert(!a1_key_light_frame(&feedback, 2001, 64, colors));
        assert(memcmp(before, colors, sizeof(before)) == 0);
    }
    /* Child-lock refusal stays red, and Wi-Fi/combination feedback survives. */
    for (unsigned key = 0; key < 3; key++) {
        a1_key_light_start(&feedback, key, key == 0, true, false, 2000);
        assert(a1_key_light_frame(&feedback, 2000, 64, colors));
        assert(colors[0] == (key == 0 ? 0x004000u : key == 1 ? 0x000040u : 0x400000u));
    }
    a1_key_light_start(&feedback, 0, false, false, true, UINT32_MAX - 79);
    assert(a1_key_light_frame(&feedback, 79, 0, colors));
    for (unsigned i = 0; i < 32; i++) assert(colors[i] == 0);
    assert(!a1_key_light_frame(&feedback, 80, 64, colors));
    /* Expired feedback leaves the next business frame intact. */
    a1_fan_frame(colors, 50, color);
    assert(!a1_key_light_frame(&feedback, 81, 64, colors));
    unsigned lit = 0;
    for (unsigned i = 0; i < 32; i++) lit += colors[i] != 0;
    assert(lit == 15);

    a1_mode_light_t mode = {0};
    colors[0] = color;
    assert(!a1_mode_light_frame(&mode, 0, true, 100, 64, colors));
    assert(colors[0] == color); /* Saved auto at boot does not flash. */
    assert(a1_mode_light_frame(&mode, 1, true, 1000, 64, colors));
    const unsigned times[] = {225, 450, 600, 825, 1050, 1199};
    for (unsigned i = 0; i < sizeof(times) / sizeof(times[0]); i++) {
        assert(a1_mode_light_frame(&mode, 1, true, 1000 + times[i], 64, colors));
        for (unsigned j = 0; j < 31; j++)
            assert(colors[j] == (i == 0 || i == 3 ? 0x280040u : 0));
        assert(colors[31] == 0);
    }
    colors[0] = color;
    assert(!a1_mode_light_frame(&mode, 1, true, 2200, 64, colors));
    assert(colors[0] == color); /* Business rendering regains the frame. */
    assert(a1_mode_light_frame(&mode, 2, false, 3000, 64, colors));
    assert(a1_mode_light_frame(&mode, 2, false, 3600, 64, colors));
    for (unsigned i = 0; i < 31; i++) assert(colors[i] == 0x1b4000);
    assert(!a1_mode_light_frame(&mode, 2, false, 4200, 64, colors));
    /* A configuration-driven transition is detected without a command event. */
    assert(a1_mode_light_frame(&mode, 2, true, 5000, 64, colors));
    assert(a1_mode_light_frame(&mode, 2, true, 5225, 0, colors));
    for (unsigned i = 0; i < 32; i++) assert(colors[i] == 0);
    /* Re-selecting the same mode restarts feedback and handles tick rollover. */
    assert(a1_mode_light_frame(&mode, 3, true, UINT32_MAX - 100, 64, colors));
    assert(a1_mode_light_frame(&mode, 3, true, 124, 16, colors));
    for (unsigned i = 0; i < 31; i++) assert(colors[i] == 0x0a0010);
    assert(!a1_mode_light_frame(&mode, 3, true, 1099, 64, colors));
    return 0;
}
