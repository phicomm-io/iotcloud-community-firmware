/* Strong overrides intentionally compile apart from production weak symbols. */
#include <stdbool.h>
#include <stdint.h>
uint32_t test_clock_ms=123000;
static bool recovery;
bool device_config_recovery(void){return recovery;}
void device_config_recovery_set(bool value){recovery=value;}

uint32_t test_heap_free=1024u*1024u;

#include <stddef.h>
size_t test_heap_live_bytes;
bool test_heap_dynamic;
