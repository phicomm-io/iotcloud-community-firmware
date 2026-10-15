#include "../common/gesture.h"
#include <assert.h>
int main(void) {
    assert(gesture_release(100,5099)==GESTURE_ORDINARY);
    assert(gesture_release(100,5100)==GESTURE_NETWORK_RESET);
    assert(gesture_release(100,10099)==GESTURE_NETWORK_RESET);
    assert(gesture_release(100,10100)==GESTURE_FULL_RESET);
    assert(gesture_release(0,60000)==GESTURE_FULL_RESET);
    assert(gesture_release(UINT32_MAX-4999u,0)==GESTURE_NETWORK_RESET);
    assert(gesture_release(UINT32_MAX-9999u,0)==GESTURE_FULL_RESET);
}
