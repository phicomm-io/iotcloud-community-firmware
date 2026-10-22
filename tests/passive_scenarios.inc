static void passive_scenarios(void) {
  transaction("binding","{\"response_ids\":[]}",405);
  transaction("linkage","{\"enabled\":true}",405);
  transaction("light","{}",405);transaction("probe","{}",405);
  transaction("time","{\"timezone_minutes\":480}",200);
  transaction("sleep","{\"windows\":[{\"days\":127,\"start\":1320,\"end\":420,\"brightness\":0}]}",200);
  feature_config_t original=stored;
  char filters[4][FEATURE_TOPIC_MAX];memset(filters,0x5a,sizeof(filters));
  assert(feature_mqtt_filters(filters,4)==0 && feature_mqtt_filters_current(filters,0));
  assert(!feature_mqtt_filters_current(filters,1) && filters[0][0]==0x5a);
  json_object *grant=obj("{\"v\":2,\"a1\":\"112233445566\",\"boot\":11,\"token\":\"abababababababababababababababab\"}");
  json_object *o=obj("{\"v\":2,\"src\":\"112233445566\",\"dst\":\"aabbccddee02\",\"op\":\"read\",\"boot\":11,\"id\":7,\"token\":\"abababababababababababababababab\"}");
  assert(!feature_bound());incoming(o,true,false,"127.0.0.1");assert(!feature_bound());
  /* The ordinary HTTP dispatcher cannot authorize sessions. */
  json_object *command=obj("{\"action\":\"sensor.session.set\",\"params\":{\"v\":2,\"a1\":\"112233445566\",\"boot\":11,\"token\":\"abababababababababababababababab\"}}");
  json_object *denied=service_command(command);assert(denied && !feature_bound());json_object_put(denied);json_object_put(command);
  /* Random boot IDs use all 32 bits; never encode them as signed JSON ints. */
  number(grant,"boot",UINT32_MAX);
  assert(json_object_get_int64(json_object_object_get(grant,"boot"))==UINT32_MAX);
  assert(feature_sensor_session_set(grant)==200);
  number(grant,"boot",11);
  assert(feature_sensor_session_set(grant)==200 && feature_bound());
  assert(feature_sensor_session_set(grant)==200); /* duplicate QoS1 grant is idempotent */
  assert(!memcmp(&stored,&original,sizeof(stored)));
  json_object *status=feature_json("status");assert(json_object_array_length(json_object_object_get(status,"peers"))==1);json_object_put(status);
  feature_sample_t sample={.value={20,50,100,.1},.sequence=9,.boot=7,.received_ms=100,.valid=true};
  feature_local_sample(&sample);test_now=200;
  int receiver=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);assert(receiver>=0);
  struct sockaddr_in address={0};address.sin_family=AF_INET;address.sin_port=htons(FEATURE_PORT);address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
  assert(bind(receiver,(struct sockaddr *)&address,sizeof(address))==0);
  udp=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);assert(udp>=0);
  for(unsigned attempt=0;attempt<32;attempt++) {
    js(o,"op",attempt%2?"probe":"read");incoming(o,true,false,"127.0.0.1");
    fd_set fds;FD_ZERO(&fds);FD_SET(receiver,&fds);struct timeval timeout={1,0};
    assert(select(receiver+1,&fds,NULL,NULL,&timeout)==1);
    char wire[FEATURE_WIRE_MAX+1];int length=recv(receiver,wire,sizeof(wire)-1,0);assert(length>0);
    json_object *reply=parse(wire,length);assert(reply && integer(reply,"v")==2 && integer(reply,"id")==7);
    assert(!strcmp(string(reply,"op"),attempt%2?"pong":"sample"));
    if(!(attempt%2))assert(integer(json_object_object_get(reply,"body"),"sequence")==9);
    json_object_put(reply);
  }
  /* MQTT reads, retained requests, old boot identities and wrong tokens cannot read or refresh. */
  uint32_t seen=sensor_sessions[0].last_seen;test_now++;
  incoming(o,false,false,NULL);incoming(o,true,true,"127.0.0.1");
  number(o,"boot",10);incoming(o,true,false,"127.0.0.1");number(o,"boot",11);
  js(o,"token","cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd");incoming(o,true,false,"127.0.0.1");
  assert(sensor_sessions[0].last_seen==seen && !queued);
  test_now=seen+SENSOR_SESSION_IDLE_MS;assert(!feature_bound());
  js(o,"token","abababababababababababababababab");incoming(o,true,false,"127.0.0.1");assert(!feature_bound());
  assert(feature_sensor_session_set(grant)==200);
  /* Bound storage: an unexpired active session is never evicted by a fifth A1. */
  for(unsigned i=1;i<=4;i++) { char id[13];snprintf(id,sizeof(id),"%012x",i);js(grant,"a1",id);
    assert(feature_sensor_session_set(grant)==(i<4?200:409)); }
  assert(!memcmp(&stored,&original,sizeof(stored)));
  js(grant,"token","00000000000000000000000000000000");assert(feature_sensor_session_set(grant)==400);
  mqtt_account_t a=mqtt_snapshot();feature_mqtt_session(&a);assert(feature_bound()); /* reconnect preserves RAM sessions */
  assert(mqtt_account_init(&a,1,"m1","different_account",LOCAL_ID));feature_mqtt_session(&a);assert(!feature_bound());
  assert(!memcmp(&stored,&original,sizeof(stored)));
  close(receiver);close(udp);udp=-1;json_object_put(o);json_object_put(grant);
  status=feature_json("binding");assert(!json_object_object_get(status,"response_ids") && integer(status,"session_protocol")==2);json_object_put(status);
  unsigned before=publications;sample.sequence++;feature_local_sample(&sample);feature_mqtt_service(capture,true);assert(publications==before);
  puts("M1 MQTT-only grants, no pairing/Flash writes, authenticated UDP, idle expiry, capacity and reconnect/account isolation passed");
}
