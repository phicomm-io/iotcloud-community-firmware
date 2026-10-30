/* Public timing semantics shared by two entirely different device runtimes. */
#include "../common/power_trigger.h"
#include <assert.h>
#include <string.h>
int main(void){
 ph_power_rule r={.id=1,.enabled=1,.comparison=PH_POWER_BELOW,.power_milliwatts=5000,.seconds=10,.target=255};
 ph_power_trigger s={0};assert(sizeof(r)==16&&ph_power_rule_valid(&r,6));
 assert(!ph_power_trigger_due(&s,&r,1000,true,4999));
 assert(!ph_power_trigger_due(&s,&r,10999,true,0));
 assert(ph_power_trigger_due(&s,&r,11000,true,0));
 /* Uncommitted busy remains retryable. A completed action is not repeated. */
 assert(ph_power_trigger_due(&s,&r,12000,true,0));ph_power_trigger_commit(&s);
 assert(!ph_power_trigger_due(&s,&r,13000,true,0));
 assert(!ph_power_trigger_due(&s,&r,14000,false,0));
 assert(!ph_power_trigger_due(&s,&r,15000,true,0));
 assert(!ph_power_trigger_due(&s,&r,16000,true,5000));
 assert(!ph_power_trigger_due(&s,&r,17000,true,0));
 assert(!ph_power_trigger_due(&s,&r,18000,false,0));
 assert(!ph_power_trigger_due(&s,&r,19000,true,0));
 assert(!ph_power_trigger_due(&s,&r,28999,true,0));
 assert(ph_power_trigger_due(&s,&r,29000,true,0));
 ph_power_trigger_override(&s);assert(!ph_power_trigger_due(&s,&r,30000,true,0));
 assert(!ph_power_trigger_due(&s,&r,31000,true,6000));
 r.seconds=0;r.comparison=PH_POWER_ABOVE;
 assert(!ph_power_trigger_due(&s,&r,32000,true,5000));
 assert(ph_power_trigger_due(&s,&r,33000,true,5001));ph_power_trigger_commit(&s);
 r.enabled=0;assert(!ph_power_trigger_due(&s,&r,34000,true,6000));
 r.enabled=1;assert(ph_power_trigger_due(&s,&r,35000,true,6000));
 memset(&s,0,sizeof(s));r.seconds=2;
 assert(!ph_power_trigger_due(&s,&r,UINT32_MAX-999,true,6000));
 assert(!ph_power_trigger_due(&s,&r,999,true,6000));
 assert(ph_power_trigger_due(&s,&r,1000,true,6000));
 r.seconds=PH_POWER_DURATION_MAX;assert(ph_power_rule_valid(&r,1));
 ++r.seconds;assert(!ph_power_rule_valid(&r,1));r.seconds=0;r.target=1;
 assert(!ph_power_rule_valid(&r,1));assert(ph_power_rule_valid(&r,6));
 r.on=2;assert(!ph_power_rule_valid(&r,6));return 0;
}
