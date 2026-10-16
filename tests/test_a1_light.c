#include <assert.h>
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
            assert(colors[(9 + 31 - d) % 31] == colors[(9 + d) % 31]);
            if (d > 1 && colors[(9 + d) % 31])
                assert(colors[(9 + d - 1) % 31] == color);
        }
    }
    a1_fan_frame(colors, 0, color);
    for (unsigned i = 0; i < 32; i++) assert(colors[i] == 0);
    a1_fan_frame(colors, 2, color);
    assert(colors[9] == half && colors[8] == 0 && colors[10] == 0);
    a1_fan_frame(colors, 6, color);
    assert(colors[9] == color && colors[8] == half && colors[10] == half);
    a1_fan_frame(colors, 10, color);
    assert(colors[9] == color && colors[8] == color && colors[10] == color);
    a1_fan_frame(colors, 99, color);
    assert(colors[24] == half && colors[25] == half);
    a1_fan_frame(colors, 150, color);
    for (unsigned i = 0; i < 31; i++) assert(colors[i] == color);
    assert(colors[31] == 0);
    a1_fan_frame(colors, 100, 0);
    for (unsigned i = 0; i < 32; i++) assert(colors[i] == 0);
    a1_fan_frame(colors, 2, 0xff0103);
    assert(colors[9] == 0x7f0001);
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
        a1_key_light_start(&feedback, kind == 1, kind == 2, false, 1000);
        assert(a1_key_light_frame(&feedback, 1000, 64, colors));
        for (unsigned i = 0; i < 32; i++) assert(colors[i] == expected[kind]);
        assert(a1_key_light_frame(&feedback, 1299, 64, colors));
        assert(!a1_key_light_frame(&feedback, 1300, 64, colors));
        assert(!feedback.active);
    }
    a1_key_light_start(&feedback, 0, false, true, UINT32_MAX - 79);
    assert(a1_key_light_frame(&feedback, 79, 0, colors));
    for (unsigned i = 0; i < 32; i++) assert(colors[i] == 0);
    assert(!a1_key_light_frame(&feedback, 80, 64, colors));
    /* Expired feedback leaves the next business frame intact. */
    a1_fan_frame(colors, 50, color);
    assert(!a1_key_light_frame(&feedback, 81, 64, colors));
    unsigned lit = 0;
    for (unsigned i = 0; i < 32; i++) lit += colors[i] != 0;
    assert(lit == 15);
    return 0;
}
