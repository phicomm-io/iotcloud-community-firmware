static void passive_scenarios(void) {
  json_object *r=NULL;
  transaction("binding","{\"response_ids\":[\"112233445566:aabbccddee02\",\"112233445566:aabbccddee02\"]}",200);
  assert(stored.peer_count==1 && !stored.peers[0].confirmed);
  transaction("binding","{\"response_ids\":[\"112233445566:000000000000\"]}",400);
  transaction("linkage","{\"enabled\":true}",405);
  transaction("light","{}",405);
  transaction("probe","{}",405);
  transaction("time","{\"timezone_minutes\":480}",200);
  transaction("sleep","{\"windows\":[{\"days\":127,\"start\":1320,\"end\":420,\"brightness\":0}]}",200);
  uint32_t revision=stored.revision;
  char filters[12][FEATURE_TOPIC_MAX];
  assert(feature_mqtt_filters(filters,sizeof(filters)/sizeof(filters[0]))==1);
  assert(!strchr(filters[0],'#') && !strchr(filters[0],'+'));
  json_object *o=envelope(&stored,0,"bind",1);
  js(o,"src",stored.peers[0].id);js(o,"dst",LOCAL_ID);
  js(o,"token","abababababababababababababababab");
  incoming(o,false,true,NULL);assert(!peers[0].session_valid);
  js(o,"op","config_set");incoming(o,false,false,NULL);assert(!peers[0].session_valid);
  js(o,"op","read");incoming(o,true,false,"127.0.0.1");assert(!peers[0].session_valid);
  strcpy(m1_state.ip,"192.0.2.42");
  js(o,"op","bind");incoming(o,false,false,NULL);
  assert(peers[0].session_valid && feature_bound() && queued==1 && stored.revision==revision);
  assert(strstr(queue[0].body,"192.0.2.42"));
  assert(!memcmp(stored.peers[0].token,(uint8_t[16]){0},16));
  feature_mqtt_service(capture,true);assert(!queued);
  feature_sample_t sample={.value={20,50,100,.1},.sequence=9,.boot=7,.received_ms=100,.valid=true};
  feature_local_sample(&sample);test_now=200;
  js(o,"op","read");incoming(o,false,false,NULL);
  assert(queued==1 && strstr(queue[0].body,"sample") && strstr(queue[0].body,"age_ms"));
  feature_mqtt_service(capture,true);assert(!queued);
  int receiver=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);assert(receiver>=0);
  struct sockaddr_in address={0};address.sin_family=AF_INET;address.sin_port=htons(FEATURE_PORT);address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
  assert(bind(receiver,(struct sockaddr *)&address,sizeof(address))==0);
  udp=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);assert(udp>=0);
  incoming(o,true,false,"127.0.0.1");
  fd_set fds;FD_ZERO(&fds);FD_SET(receiver,&fds);struct timeval timeout={1,0};
  assert(select(receiver+1,&fds,NULL,NULL,&timeout)==1);
  char wire[FEATURE_WIRE_MAX+1];int length=recv(receiver,wire,sizeof(wire)-1,0);assert(length>0);wire[length]=0;
  json_object *reply=parse(wire,length);assert(reply && !strcmp(string(reply,"op"),"sample"));
  assert(integer(json_object_object_get(reply,"body"),"sequence")==9);json_object_put(reply);
  /* Repeated manual probes and sample reads share the UDP reply path. */
  for (unsigned attempt=0; attempt<32; attempt++) {
    const char *op=attempt%2 ? "read" : "probe";
    js(o,"op",op);incoming(o,true,false,"127.0.0.1");
    FD_ZERO(&fds);FD_SET(receiver,&fds);timeout=(struct timeval){1,0};
    assert(select(receiver+1,&fds,NULL,NULL,&timeout)==1);
    length=recv(receiver,wire,sizeof(wire)-1,0);assert(length>0);
    reply=parse(wire,length);assert(reply);
    assert(!strcmp(string(reply,"op"),attempt%2 ? "sample" : "pong"));
    json_object_put(reply);
  }
  char oversized[FEATURE_WIRE_MAX+1];memset(oversized,'x',sizeof(oversized)-1);oversized[sizeof(oversized)-1]=0;
  json_object *large=json_object_new_object();js(large,"body",oversized);
  assert(!send_udp("127.0.0.1",large));json_object_put(large);
  assert(stored.revision==revision && stored.peer_count==1);
  close(receiver);close(udp);udp=-1;
  js(o,"token","cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd");incoming(o,false,false,NULL);assert(!queued);
  js(o,"token","abababababababababababababababab");
  js(o,"op","config_get");incoming(o,false,false,NULL);assert(!queued);
  js(o,"op","unbind");incoming(o,false,false,NULL);assert(stored.peer_count==1 && stored.revision==revision);
  js(o,"src","000000000001");js(o,"op","bind");incoming(o,false,false,NULL);assert(!queued);
  json_object_put(o);
  r=feature_json("binding");assert(json_object_object_get(r,"response_ids"));assert(!json_object_object_get(r,"peers"));json_object_put(r);
  /* New sensor data must never create unsolicited linkage publications. */
  unsigned before=publications;test_now=300;sample.sequence++;feature_local_sample(&sample);
  feature_mqtt_service(capture,true);assert(publications==before);
  puts("M1 response IDs, RAM sessions, passive reads, forbidden writes and no unsolicited mirror passed");
}
