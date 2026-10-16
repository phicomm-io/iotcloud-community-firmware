#include <assert.h>
#include <stdio.h>
#include "../A1/ring_feedback.h"

int main(void)
{
    uint32_t previous[32] = {0}, frame[32];
    for (unsigned speed = 0; speed <= 100; speed++) {
        a1_ring_feedback(frame, speed, 255, 0xffffff);
        assert(frame[0] == 0);
        unsigned partial = 0;
        for (unsigned distance = 1; distance <= 15; distance++) {
            unsigned left = (10 + 31 - distance - 1) % 31 + 1;
            unsigned right = (10 + distance - 1) % 31 + 1;
            unsigned a = (left + 16) % 31 + 1, b = (right + 16) % 31 + 1;
            assert(frame[a] == frame[b]);
            assert(frame[a] >= previous[a]);
            unsigned level = frame[a] & 255;
            partial += level > 0 && level < 255;
            if (speed == 100) assert(level == 255);
        }
        assert(partial <= 2);
        if (speed) {
            assert(frame[27] == 0xffffff); /* LED 10 stays at six o'clock. */
            assert(memcmp(frame, previous, sizeof(frame)) != 0);
        } else {
            for (unsigned i = 0; i < 32; i++) assert(frame[i] == 0);
        }
        memcpy(previous, frame, sizeof(frame));
    }
    assert(a1_ring_level(90, 15) < 255);
    assert(a1_ring_level(99, 15) < 255);
    a1_ring_feedback(frame, 100, 0, 0xffffff);
    for (unsigned i = 0; i < 32; i++) assert(frame[i] == 0);
    puts("A1 ring: 1% resolution, symmetric two-pair edge, full scale and blackout passed");
}
