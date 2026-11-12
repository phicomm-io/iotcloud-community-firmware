/* Historical runtime scenarios, migrated only at MP input/output seams. */
static void effective_controls(void) {
  effective_on=true;
  feature_config_t c = stored;
  c.light.effect = EFFECT_SOLID;
  assert(install_config(&c));
  uint32_t pixels[32] = {0};
  sleep_state = (feature_sleep_state_t){0};
  feature_light_frame(pixels, 255, true, 255);
  for (unsigned i = 0; i < 32; i++)
    assert(pixels[i] == 255);
  sleep_state.light_sleep = true;
  sleep_state.brightness = 0;
  feature_light_frame(pixels, 255, true, 255);
  for (unsigned i = 0; i < 32; i++)
    assert(pixels[i] == 0);
  sleep_state.brightness = 16;
  feature_light_frame(pixels, 128, true, 255);
  for (unsigned i = 0; i < 32; i++)
    assert(pixels[i] == 16);
  configuration.light.effect = EFFECT_FAN;
  for (unsigned i = 0; i < 32; i++)
    pixels[i] = 128;
  feature_light_frame(pixels, 128, true, 255);
  for (unsigned i = 0; i < 32; i++)
    assert(pixels[i] == 16);
  sleep_state.fan_sleep = true;
  sleep_state.max_speed = 30;
  bool on;
  unsigned speed;
  feature_effective_fan(true, 100, &on, &speed);
  assert(on && speed == 30);
  /* A status copy must report the last applied controller result, never
     advance PID or recompute from a newly requested setting. */
  { bool applied,automatic;unsigned percentage,limit;
    __typeof__(pid) before=pid;bool valid=automatic_valid;
    uint32_t clock=test_now;
    feature_fan_snapshot(&applied,&percentage,&automatic,&limit);
    assert(applied==on && percentage==speed);
    assert(!memcmp(&before,&pid,sizeof(pid)) && automatic_valid==valid && test_now==clock);
    effective_on=false;effective_speed=17;
    feature_fan_snapshot(&applied,&percentage,&automatic,&limit);
    assert(!applied && percentage==17); /* requested state is not consulted */
    effective_on=on;effective_speed=speed;
  }
  c = stored;
  c.scheduled_off = 1;
  c.scheduled_valid = 1;
  c.last_event = 123;
  assert(install_config(&c));
  feature_effective_fan(true, 100, &on, &speed);
  assert(on && speed == 30); /* Old timer latch never overrides a new command. */
  feature_manual_fan(true);
  assert(!stored.scheduled_off && !stored.scheduled_valid &&
         stored.last_event == 123);
  sleep_state = (feature_sleep_state_t){0};
  configuration.light = stored.light;
  unsigned revision = stored.revision;
  for (unsigned i = 0; i < 1000; i++)
    feature_effective_fan(true, 60, &on, &speed);
  assert(stored.revision == revision);
  puts("Effective fan cap, persisted timer latch, manual resume and light "
       "sleep priority passed");
}
static void automatic_profiles(void) {
  feature_config_t c=stored;
  c.linkage.enabled=1;c.linkage.min_speed=10;c.linkage.max_speed=80;
  c.automatic.fallback_speed=90;c.sleep_count=1;
  c.sleeps[0].max_speed=35;c.automatic.night[0].min_speed=0;
  c.automatic.night[0].fallback_speed=0;
  assert(install_config(&c));data_valid=false;automatic_night=-1;
  bool on;unsigned speed;
  feature_effective_fan(true,64,&on,&speed);assert(on && speed==80);
  automatic_night=0;automatic_valid=false;automatic_immediate=true;
  feature_effective_fan(true,64,&on,&speed);assert(on && speed==0);
  feature_effective_fan(false,64,&on,&speed);assert(!on && speed==0);
  ph_value command=obj("{\"speed\":22}");
  assert(feature_fan_command_mp(&command)==2);
  assert(stored.sleeps[0].max_speed==22 && stored.linkage.max_speed==80);
  assert(feature_fan_limit()==22);
  automatic_night=-1;automatic_valid=false;automatic_immediate=true;
  feature_effective_fan(true,64,&on,&speed);assert(on && speed==80);
  c=stored;c.linkage.hysteresis_enabled=1;c.linkage.mode=0;
  c.linkage.fixed_speed=55;c.linkage.on_pm=50;c.linkage.target_pm=35;
  c.linkage.debounce_seconds=5;c.linkage.min_switch_seconds=10;
  assert(install_config(&c));data_valid=true;available=1;aggregate[2]=40;
  feature_effective_fan(true,64,&on,&speed);assert(on && speed==0);
  aggregate[2]=50;automatic_valid=false;test_now=1000;
  feature_effective_fan(true,64,&on,&speed);assert(on && speed==0);
  automatic_valid=false;test_now=6000;
  feature_effective_fan(true,64,&on,&speed);assert(on && speed==55);
  aggregate[2]=40;
  command=obj("{\"speed\":45}");assert(feature_fan_command_mp(&command)==2);
  feature_manual_fan(true);
  feature_effective_fan(true,64,&on,&speed);assert(on && speed==45 && pid.gate_running);
  aggregate[2]=35;test_now=7000;automatic_valid=false;
  feature_effective_fan(true,64,&on,&speed);assert(speed==45);
  test_now=16000;automatic_valid=false;
  feature_effective_fan(true,64,&on,&speed);assert(on && speed==0);
  /* Missing sensor overrides the gate; recovery cancels pending conditions. */
  data_valid=false;automatic_valid=false;
  feature_effective_fan(true,64,&on,&speed);assert(speed==45);
  data_valid=true;aggregate[2]=40;automatic_valid=false;
  feature_effective_fan(true,64,&on,&speed);assert(on && speed==0);
  feature_manual_fan(false);assert(!pid.gate_running && !pid.switched);
  c=stored;c.linkage.enabled=0;c.linkage.hysteresis_enabled=0;c.sleep_count=0;assert(install_config(&c));
  puts("Independent day/night automatic ranges, zero fallback, power latch and active cap passed");
}
static void animation_speed(void) {
  feature_config_t original=configuration;
  uint32_t original_ms=test_now, normal[32], fast[32], slow[32];
  effective_on=true;
  configuration.light.follow_fan=0;
  const unsigned animated[]={EFFECT_BREATHE,EFFECT_RAINBOW,EFFECT_COLOR_BREATHE};
  for(unsigned k=0;k<sizeof(animated)/sizeof(animated[0]);k++){
    unsigned effect=animated[k];
    configuration.light.effect=effect;configuration.light.speed=0;test_now=3000;
    feature_light_frame(normal,255,true,0x1122ff);
    configuration.light.speed=5;feature_light_frame(fast,255,true,0x1122ff);
    assert(!memcmp(normal,fast,sizeof(normal))); /* zero-filled legacy byte */
    configuration.light.speed=10;test_now=1500;feature_light_frame(fast,255,true,0x1122ff);
    assert(!memcmp(normal,fast,sizeof(normal)));
    configuration.light.speed=1;test_now=15000;feature_light_frame(slow,255,true,0x1122ff);
    assert(!memcmp(normal,slow,sizeof(normal)));
    test_now=3000;feature_light_frame(slow,255,true,0x1122ff);
    assert(memcmp(normal,slow,sizeof(normal)));
  }
  /* Every business effect is dark while powered off, independent of follow.
   * Powered automatic idle is a separate state and keeps custom brightness. */
  for(unsigned effect=EFFECT_FAN;effect<=EFFECT_COLOR_BREATHE;effect++){
    configuration.light.effect=effect;
    for(unsigned follow=0;follow<2;follow++){
      configuration.light.follow_fan=follow;effective_on=false;
      for(unsigned i=0;i<32;i++)normal[i]=0xffffff;
      feature_light_frame(normal,255,true,0x1122ff);
      for(unsigned i=0;i<32;i++)assert(normal[i]==0);
    }
  }
  effective_on=true;effective_speed=0;configuration.light.follow_fan=0;
  configuration.light.effect=EFFECT_SOLID;
  feature_light_frame(normal,128,true,0x0000ff);
  assert(normal[0]==128);
  configuration.light.effect=EFFECT_COLOR_BREATHE;configuration.light.speed=5;
  test_now=1000;feature_light_frame(normal,128,true,0x1122ff);
  test_now=2000;feature_light_frame(fast,128,true,0x1122ff);
  assert(normal[0] && fast[0] && normal[0]!=fast[0]);
  for(unsigned i=0;i<32;i++)assert(normal[i]==normal[0]);
  configuration=original;test_now=original_ms;
  transaction("light","{\"effect\":5}",200);assert(stored.light.effect==EFFECT_COLOR_BREATHE);
  assert(select_effect(255,true)==200);
  assert(stored.light.effect==EFFECT_FAN);
  assert(select_effect(5,false)==200 && stored.light.effect==EFFECT_COLOR_BREATHE);
  assert(select_effect(6,false)==400);
  transaction("light","{\"effect\":6}",400);
  stored.light.speed=0;encoded_speed_default();
  transaction("light","{\"speed\":0}",400);
  transaction("light","{\"speed\":11}",400);
  transaction("light","{\"speed\":1.5}",400);
  transaction("light","{\"speed\":true}",400);
  transaction("light","{\"speed\":1}",200);assert(stored.light.speed==1);
  transaction("light","{\"speed\":10}",200);assert(stored.light.speed==10);
  transaction("light","{\"effect\":2}",200);assert(stored.light.speed==10);
  puts("Animation speed: legacy default, render timing, bounds and persistence passed");
}
