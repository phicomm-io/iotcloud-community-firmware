/* Strong overrides intentionally compile apart from production weak symbols. */
#include <stdbool.h>
#include <stdint.h>
uint32_t test_clock_ms=123000;
static bool recovery;
bool device_config_recovery(void){return recovery;}
void device_config_recovery_set(bool value){recovery=value;}
