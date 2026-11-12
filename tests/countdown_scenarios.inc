/* Exercise the real dispatcher and scheduler with an independent uptime clock. */
{
 automation_defaults(&stored);memset(countdowns,0,sizeof(countdowns));countdown_ready=false;calls=0;
 const char *start="{\"action\":\"schedules.put\",\"params\":{\"enabled\":true,\"trigger\":\"countdown\",\"seconds\":60,\"action\":\"fan.set\",\"params\":{\"on\":false}}}";
 const char *restart="{\"action\":\"schedules.enable\",\"params\":{\"id\":1,\"enabled\":true}}";
 const char *cancel="{\"action\":\"schedules.enable\",\"params\":{\"id\":1,\"enabled\":false}}";
 ph_value r=request(start,"accepted"),d=r;
 assert(mp_int(mp_get(d,"remaining_seconds"))==60);mp_release(r);
 assert(stored.tasks[0].once==2 && stored.tasks[0].at==60);
 epoch=0;test_now+=59000;automation_poll(false);assert(!calls);
 /* A forward/backward wall-clock correction cannot change the deadline. */
 epoch=2000000000;test_now+=1000;automation_poll(false);assert(calls==1 && !stored.tasks[0].enabled && stored.tasks[0].state==AUTO_EXECUTED);
 automation_poll(true);assert(calls==1);
 CHECK(restart,"accepted");test_now+=30000;automation_poll(false);assert(calls==1);
 CHECK(cancel,"accepted");test_now+=90000;automation_poll(false);assert(calls==1);
 CHECK(restart,"accepted");test_now+=59000;automation_poll(false);assert(calls==1);
 test_now+=1000;automation_poll(false);assert(calls==2);
 /* Uptime wrap, multiple tasks and edit/reset of an active deadline. */
 test_now=UINT32_MAX-30000;CHECK(restart,"accepted");CHECK(start,"accepted");
 test_now+=59000;automation_poll(false);assert(calls==2);
 test_now+=1000;automation_poll(false);assert(calls==4);
 CHECK(restart,"accepted");test_now+=30000;
 CHECK("{\"action\":\"schedules.put\",\"params\":{\"id\":1,\"enabled\":true,\"trigger\":\"countdown\",\"seconds\":90,\"action\":\"fan.set\",\"params\":{\"on\":false}}}","accepted");
 test_now+=60000;automation_poll(false);assert(calls==4);test_now+=30000;automation_poll(false);assert(calls==5);
 /* Commit failure must not arm a new task or execute an expired one. */
 fail_save=true;CHECK(restart,"storage_error");fail_save=false;test_now+=90000;automation_poll(false);assert(calls==5);
 CHECK(restart,"accepted");test_now+=90000;fail_save=true;automation_poll(false);assert(calls==5 && stored.tasks[0].enabled);
 fail_save=false;automation_poll(false);assert(calls==6 && !stored.tasks[0].enabled);
 CHECK(restart,"accepted");busy=true;test_now+=90000;automation_poll(false);assert(calls==6 && stored.tasks[0].state==AUTO_BUSY && !stored.tasks[0].enabled);busy=false;
 CHECK(restart,"accepted");CHECK("{\"action\":\"schedules.delete\",\"params\":{\"id\":1}}","accepted");test_now+=90000;automation_poll(false);assert(calls==6);
 CHECK(start,"accepted");
 /* Simulate volatile state lost at boot: cancel before any command/poll. */
 countdown_ready=false;memset(countdowns,0,sizeof(countdowns));test_now=0;
 fail_save=true;automation_poll(false);assert(stored.tasks[0].enabled);fail_save=false;
 automation_poll(false);assert(!stored.tasks[0].enabled && stored.tasks[0].state==AUTO_CANCELLED && calls==6);
 CHECK("{\"action\":\"schedules.put\",\"params\":{\"enabled\":true,\"trigger\":\"countdown\",\"seconds\":0,\"action\":\"fan.set\",\"params\":{\"on\":false}}}","invalid_params");
 CHECK("{\"action\":\"schedules.put\",\"params\":{\"enabled\":true,\"trigger\":\"countdown\",\"seconds\":86401,\"action\":\"fan.set\",\"params\":{\"on\":false}}}","invalid_params");
 CHECK("{\"action\":\"schedules.put\",\"params\":{\"enabled\":true,\"trigger\":\"countdown\",\"seconds\":60,\"action\":\"fan.set\",\"params\":{\"on\":true}}}","invalid_params");
 CHECK("{\"action\":\"schedules.put\",\"params\":{\"enabled\":true,\"trigger\":\"countdown\",\"seconds\":60,\"action\":\"fan.set\",\"params\":{\"on\":false,\"speed\":50}}}","invalid_params");
 CHECK("{\"action\":\"schedules.put\",\"params\":{\"enabled\":true,\"trigger\":\"countdown\",\"seconds\":1,\"action\":\"fan.set\",\"params\":{\"on\":false}}}","accepted");
 test_now+=1000;automation_poll(false);assert(calls==7);
 CHECK("{\"action\":\"schedules.put\",\"params\":{\"enabled\":true,\"trigger\":\"countdown\",\"seconds\":86400,\"action\":\"fan.set\",\"params\":{\"on\":false}}}","accepted");
 puts("PASS countdown: no clock, corrections, cancel/restart, edit, concurrent tasks, rollover, persistence, busy, reboot cancellation and bounds");
}
