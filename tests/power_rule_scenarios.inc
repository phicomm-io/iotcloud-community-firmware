/* Exercise the public MessagePack API and real TC1 scheduler without an RTC. */
 automation_defaults(&stored);memset(power_states,0,sizeof(power_states));calls=0;
 power_valid=true;power_mw=120000;test_now+=1000;
 CHECK("{\"action\":\"schedules.put\",\"params\":{\"enabled\":true,\"trigger\":\"power\",\"comparison\":\"above\",\"power_milliwatts\":100000,\"seconds\":2,\"action\":\"outlet.set\",\"params\":{\"index\":2,\"on\":false}}}","accepted");
 assert(sizeof(automation_task_t)==24 && stored.tasks[0].once==3 && stored.tasks[0].at==100000 && stored.tasks[0].last==2);
 automation_poll(false);test_now+=1000;automation_poll(false);assert(!calls);
 test_now+=1000;busy=true;automation_poll(false);assert(!calls);
 busy=false;test_now+=1000;automation_poll(false);assert(calls==1&&order[0]==2);
 test_now+=1000;automation_poll(false);assert(calls==1);
 power_valid=false;test_now+=1000;automation_poll(false);power_valid=true;
 test_now+=1000;automation_poll(false);assert(calls==1); // Missing data cannot rearm a fired rule.
 power_mw=100000;test_now+=1000;automation_poll(false); // Equality is not above.
 power_mw=120000;test_now+=1000;automation_poll(false);
 automation_power_manual_override();test_now+=2000;automation_poll(false);assert(calls==1);
 power_mw=0;test_now+=1000;automation_poll(false);power_mw=120000;
 test_now+=1000;automation_poll(false);test_now+=1000;automation_poll(false);
 power_valid=false;test_now+=1000;automation_poll(false);power_valid=true;
 for(unsigned i=0;i<3;i++){test_now+=1000;automation_poll(false);assert(calls==(i==2?2u:1u));}
 ph_value power_list=request("{\"action\":\"schedules.get\",\"params\":{}}","ok");
 ph_value power_item=mp_at(mp_get(power_list,"items"),0);
 assert(!strcmp(mp_text(mp_get(power_item,"trigger")),"power"));
 assert(!strcmp(mp_text(mp_get(power_item,"comparison")),"above"));
 assert(mp_int(mp_get(power_item,"power_milliwatts"))==100000 && mp_int(mp_get(power_item,"seconds"))==2);mp_release(power_list);
 CHECK("{\"action\":\"schedules.enable\",\"params\":{\"id\":1,\"enabled\":false}}","accepted");
 power_mw=0;test_now+=1000;automation_poll(false);power_mw=120000;test_now+=1000;automation_poll(false);assert(calls==2);
 CHECK("{\"action\":\"schedules.put\",\"params\":{\"id\":1,\"enabled\":true,\"trigger\":\"power\",\"comparison\":\"below\",\"power_milliwatts\":5000,\"seconds\":0,\"action\":\"outlet.set\",\"params\":{\"on\":false}}}","accepted");
 power_mw=4000;test_now+=1000;automation_poll(false);assert(calls==3&&order[2]==255);
 fail_save=true;CHECK("{\"action\":\"schedules.delete\",\"params\":{\"id\":1}}","storage_error");assert(stored.tasks[0].id);fail_save=false;
 CHECK("{\"action\":\"schedules.delete\",\"params\":{\"id\":1}}","accepted");
 CHECK("{\"action\":\"schedules.put\",\"params\":{\"enabled\":true,\"trigger\":\"power\",\"comparison\":\"above\",\"power_milliwatts\":0,\"seconds\":0,\"action\":\"system.reboot\",\"params\":{}}}","invalid_params");
 CHECK("{\"action\":\"schedules.put\",\"params\":{\"enabled\":true,\"trigger\":\"power\",\"comparison\":\"above\",\"power_milliwatts\":0,\"seconds\":604801,\"action\":\"outlet.set\",\"params\":{\"on\":false}}}","invalid_params");
 CHECK("{\"action\":\"schedules.put\",\"params\":{\"enabled\":true,\"trigger\":\"power\",\"comparison\":\"above\",\"power_milliwatts\":true,\"seconds\":0,\"action\":\"outlet.set\",\"params\":{\"on\":false}}}","invalid_params");
 automation_defaults(&stored);
 for(unsigned i=0;i<PH_POWER_RULES_MAX;i++){
  stored.tasks[i]=(automation_task_t){.id=i+1,.enabled=1,.once=3,.at=100000,.action={.kind=AUTO_OUTLET,.target=0,.value=0}};
 }
 stored.next_id=9;
 CHECK("{\"action\":\"schedules.put\",\"params\":{\"enabled\":true,\"trigger\":\"power\",\"comparison\":\"above\",\"power_milliwatts\":0,\"seconds\":0,\"action\":\"outlet.set\",\"params\":{\"on\":false}}}","capacity");
