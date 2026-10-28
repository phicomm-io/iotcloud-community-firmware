/* Strong overrides intentionally compile apart from production weak symbols. */
#include <stdbool.h>
static bool recovery;
bool device_config_recovery(void){return recovery;}
void device_config_recovery_set(bool value){recovery=value;}
