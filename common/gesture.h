#pragma once
#include <stdint.h>
enum { GESTURE_ORDINARY, GESTURE_NETWORK_RESET, GESTURE_FULL_RESET };
/* Evaluate on release only. Unsigned elapsed ticks survive RTOS wraparound. */
static inline unsigned gesture_release(uint32_t pressed_at, uint32_t released_at)
{
    uint32_t elapsed = released_at - pressed_at;
    return elapsed >= 10000u ? GESTURE_FULL_RESET :
           elapsed >= 5000u ? GESTURE_NETWORK_RESET : GESTURE_ORDINARY;
}
#include <stdbool.h>
#include <string.h>
enum { KEY_EVENT_NONE, KEY_EVENT_CLICKS, KEY_EVENT_HOLD, KEY_EVENT_NETWORK_RESET, KEY_EVENT_FULL_RESET };
typedef struct { bool candidate, stable, blocked; uint8_t samples, clicks; uint32_t edge_at, pressed_at, released_at; } key_gesture_t;
typedef struct { unsigned type, count; } key_event_t;
/* Two successive equal samples. Failed panel reads cancel the whole sequence. */
static inline key_event_t key_gesture_sample(key_gesture_t *g,bool valid,bool down,uint32_t now,bool recovery) {
    key_event_t out={KEY_EVENT_NONE,0};
    if(!valid){memset(g,0,sizeof(*g));g->blocked=true;return out;}
    if(down!=g->candidate){g->candidate=down;g->samples=1;g->edge_at=now;}
    else if(g->samples<2)g->samples++;
    if(g->samples>=2 && down!=g->stable){
        g->stable=down;
        if(down){g->pressed_at=g->edge_at;}
        else if(g->blocked){g->blocked=false;g->clicks=0;}
        else {
            uint32_t duration=g->edge_at-g->pressed_at;g->released_at=g->edge_at;
            if(recovery && duration>=5000){g->clicks=0;out.type=duration>=10000?KEY_EVENT_FULL_RESET:KEY_EVENT_NETWORK_RESET;}
            else if(duration>=1000){g->clicks=0;if(duration<5000){out.type=KEY_EVENT_HOLD;out.count=duration/1000;}}
            else if(g->clicks<30)g->clicks++;
        }
    }
    if(g->blocked && !down && g->samples>=2){g->blocked=false;g->stable=false;}
    if(!g->stable && !g->candidate && g->clicks && (uint32_t)(now-g->released_at)>=300){out.type=KEY_EVENT_CLICKS;out.count=g->clicks;g->clicks=0;}
    return out;
}

/* One visible pulse at every complete second while held, including 5/10 s.
 * Feedback does not alter the stored business LED state or execute actions. */
static inline bool key_hold_feedback(const key_gesture_t *g,uint32_t now) {
    uint32_t elapsed=now-g->pressed_at;
    return g->stable && g->candidate && !g->blocked && elapsed>=1000u && elapsed%1000u<160u;
}
