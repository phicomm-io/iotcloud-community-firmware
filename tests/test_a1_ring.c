#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../A1/ring_feedback.h"

int main(void)
{
    uint32_t frame[32], touch[32], previous[32] = {0};
    const uint32_t color = 0x123456;
    const unsigned brightness[] = {0, 1, 64, 128, 255};
    for (unsigned b = 0; b < sizeof(brightness) / sizeof(brightness[0]); b++) {
        unsigned level = brightness[b];
        uint32_t scaled = (((color >> 16 & 255) * level / 255) << 16) |
            (((color >> 8 & 255) * level / 255) << 8) |
            ((color & 255) * level / 255);
        for (unsigned speed = 0; speed <= 150; speed++) {
            a1_ring_feedback(frame, speed, level, color);
            a1_fan_frame(touch, speed, scaled);
            assert(memcmp(frame, touch, sizeof(frame)) == 0);
            assert(frame[31] == 0); /* No unsent 32nd position. */
        }
    }
    for (unsigned speed = 0; speed <= 100; speed++) {
        a1_ring_feedback(frame, speed, 255, 0xffffff);
        for (unsigned i = 0; i < 31; i++) assert(frame[i] >= previous[i]);
        for (unsigned d = 1; d <= 15; d++)
            assert(frame[(10 + 31 - d) % 31] == frame[(10 + d) % 31]);
        memcpy(previous, frame, sizeof(frame));
    }
    a1_ring_feedback(frame, 2, 255, 0xffffff);
    assert(frame[10] == 0x7f7f7f);
    for (unsigned i = 0; i < 32; i++) if (i != 10) assert(frame[i] == 0);
    a1_ring_feedback(frame, 6, 255, 0xffffff);
    assert(frame[10] == 0xffffff && frame[9] == 0x7f7f7f && frame[11] == 0x7f7f7f);
    a1_ring_feedback(frame, 100, 255, 0xffffff);
    for (unsigned i = 0; i < 31; i++) assert(frame[i] == 0xffffff);
    assert(frame[31] == 0);
    a1_ring_feedback(frame, 0, 255, 0xffffff);
    for (unsigned i = 0; i < 32; i++) assert(frame[i] == 0);
    puts("A1 speed ring matches original touch origin, growth, brightness and boundaries");
}
