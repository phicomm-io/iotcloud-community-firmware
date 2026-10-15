#include "../common/gesture.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    assert(gesture_release(100,5099)==GESTURE_ORDINARY);
    assert(gesture_release(100,5100)==GESTURE_NETWORK_RESET);
    assert(gesture_release(100,10099)==GESTURE_NETWORK_RESET);
    assert(gesture_release(100,10100)==GESTURE_FULL_RESET);
    assert(gesture_release(0,60000)==GESTURE_FULL_RESET);
    assert(gesture_release(UINT32_MAX-4999u,0)==GESTURE_NETWORK_RESET);
    assert(gesture_release(UINT32_MAX-9999u,0)==GESTURE_FULL_RESET);
    key_gesture_t g={0};key_event_t e;
    assert(!key_gesture_sample(&g,true,true,100,true).type);
    assert(!key_gesture_sample(&g,true,false,110,true).type); /* bounce */
    key_gesture_sample(&g,true,false,120,true);
    key_gesture_sample(&g,true,true,200,true);key_gesture_sample(&g,true,true,210,true);
    key_gesture_sample(&g,true,false,250,true);key_gesture_sample(&g,true,false,260,true);
    assert(!key_gesture_sample(&g,true,false,549,true).type);
    e=key_gesture_sample(&g,true,false,550,true);assert(e.type==KEY_EVENT_CLICKS && e.count==1);
    key_gesture_sample(&g,true,true,1000,true);key_gesture_sample(&g,true,true,1010,true);
    assert(!key_gesture_sample(&g,true,true,12000,true).type); /* release only */
    key_gesture_sample(&g,true,false,12010,true);e=key_gesture_sample(&g,true,false,12020,true);
    assert(e.type==KEY_EVENT_FULL_RESET);
    key_gesture_sample(&g,true,true,13000,true);key_gesture_sample(&g,true,true,13010,true);
    key_gesture_sample(&g,false,true,14000,true);
    key_gesture_sample(&g,true,false,14010,true);assert(!key_gesture_sample(&g,true,false,14020,true).type);
    assert(!key_gesture_sample(&g,true,false,15000,true).type);
    key_gesture_sample(&g,true,true,16000,false);key_gesture_sample(&g,true,true,16010,false);
    key_gesture_sample(&g,true,false,22000,false);assert(!key_gesture_sample(&g,true,false,22010,false).type);
    g=(key_gesture_t){.candidate=true,.stable=true,.pressed_at=100};
    assert(!key_hold_feedback(&g,1099));assert(key_hold_feedback(&g,1100));
    assert(key_hold_feedback(&g,1259));assert(!key_hold_feedback(&g,1260));
    assert(key_hold_feedback(&g,2100));assert(key_hold_feedback(&g,5100));assert(key_hold_feedback(&g,10100));
    g.stable=false;assert(!key_hold_feedback(&g,10100));
    g=(key_gesture_t){.candidate=true,.stable=true,.pressed_at=UINT32_MAX-999u};assert(key_hold_feedback(&g,0));
    puts("Gesture debounce, release-only recovery, failed-read cancellation and reserved holds passed");
}
