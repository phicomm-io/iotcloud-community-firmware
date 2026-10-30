/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Included after group implementation: all views expire before returning. */
#ifdef FEATURE_DEVICE_A1
static void feature_average_fields_mp(ph_writer *w,const feature_sample_t *current,unsigned count,uint32_t now){
 float mean[4]={NAN,NAN,NAN,NAN},co2=0;unsigned n=feature_aggregate(current,count,now,STAT_MEAN,mean,NULL);
 unsigned nc=feature_co2_aggregate(current,count,now,STAT_MEAN,&co2);bool supported=false;
 for(unsigned i=0;i<count&&i<FEATURE_MAX_PEERS;i++)supported|=current[i].co2_supported;
 phw_uint(w,"sample_count",n);phw_bool(w,"co2_supported",supported);phw_bool(w,"co2_valid",nc!=0);phw_uint(w,"co2_sample_count",nc);
 if(nc)phw_number(w,"co2",co2);else phw_null(w,"co2");
 phw_uint(w,"configured_count",count);phw_bool(w,"valid",n!=0);
 const char *keys[]={"temperature","humidity","pm25","formaldehyde"};bool complete=n!=0;
 for(unsigned i=0;i<4;i++)complete=complete&&isfinite(mean[i]);
 for(unsigned i=0;i<4;i++){if(n&&isfinite(mean[i]))phw_number(w,keys[i],mean[i]);else phw_null(w,keys[i]);}
 if(!complete){phw_begin(w,"fields",false);for(unsigned i=0;i<4;i++)phw_bool(w,keys[i],n&&isfinite(mean[i]));phw_bool(w,"co2",nc!=0);phw_end(w);}
}
#endif
bool feature_domain_get_mp(const char *d,ph_writer *w){
 feature_config_t *c=malloc(sizeof(*c));if(!c)return false;feature_config_get(c);
 phw_begin(w,NULL,false);
#ifdef FEATURE_DEVICE_A1
 if(!strcmp(d,"average")){
  feature_sample_t current[FEATURE_MAX_PEERS];lock();memcpy(current,samples,sizeof(current));unlock();feature_average_fields_mp(w,current,c->peer_count,now_ms());
 }else
#endif
 if(!strcmp(d,"status")){
  lock();uint32_t now=now_ms();phw_uint(w,"available",available);phw_uint(w,"co2_available",available_co2);
  phw_bool(w,"data_valid",data_valid);phw_bool(w,"clock_valid",clock_ok);phw_bool(w,"automatic_on",effective_on);phw_uint(w,"automatic_speed",automatic_speed);
  phw_begin(w,"peers",true);
#ifdef FEATURE_DEVICE_A1
  for(unsigned i=0;i<c->peer_count;i++){
   phw_begin(w,NULL,false);phw_text(w,"mac",c->peers[i].id);phw_bool(w,"session_ready",peers[i].authorized||samples[i].partial);
   phw_bool(w,"co2_supported",samples[i].co2_supported);phw_bool(w,"co2_valid",feature_co2_fresh(&samples[i],now));
   phw_text(w,"ip",c->peers[i].ip[0]?c->peers[i].ip:peers[i].ip);
   bool fresh=feature_sample_fresh(&samples[i],now)||feature_co2_fresh(&samples[i],now);
   phw_text(w,"transport",peers[i].lan?"lan":fresh?"mqtt":"offline");phw_bool(w,"pending",peers[i].pending);phw_bool(w,"fresh",fresh);phw_end(w);
  }
#else
  for(unsigned i=0;i<FEATURE_MAX_PEERS;i++){
   sensor_session_t *s=&sensor_sessions[i];if(!s->valid||(uint32_t)(now-s->last_seen)>=SENSOR_SESSION_IDLE_MS)continue;
   phw_begin(w,NULL,false);phw_text(w,"mac",s->id);phw_text(w,"ip",s->ip);uint32_t idle=now-(s->active?s->last_read:s->last_seen);
   phw_bool(w,"session_ready",s->active&&idle<FEATURE_STALE_MS);phw_uint(w,"idle_ms",idle);phw_end(w);
  }
#endif
  phw_end(w);
#ifdef FEATURE_DEVICE_A1
  phw_begin(w,"average",false);feature_average_fields_mp(w,samples,c->peer_count,now);phw_end(w);
  phw_begin(w,"members",true);
  for(unsigned i=0;i<c->member_count;i++){
   phw_begin(w,NULL,false);phw_text(w,"mac",c->members[i].id);phw_text(w,"ip",c->members[i].ip[0]?c->members[i].ip:group_members[i].ip);
   phw_bool(w,"pending",group_members[i].pending!=NULL);phw_text(w,"transport",group_members[i].lan&&!c->group_force_mqtt?"lan":"mqtt");phw_uint(w,"timeouts",group_members[i].failures);phw_end(w);
  }phw_end(w);
#else
  phw_uint(w,"max_sessions",FEATURE_MAX_PEERS);
#endif
  phw_text(w,"linkage_status",!c->linkage.enabled?"disabled":data_valid?"active":"sensor_failed");unlock();
 }else{
#ifndef FEATURE_DEVICE_A1
  if(strcmp(d,"binding"))
#endif
  if(!feature_config_fields_mp(c,d,w)){free(c);phw_end(w);return false;}
 }
 if(!strcmp(d,"binding")){phw_text(w,"sensor_transport","lan");phw_uint(w,"session_protocol",3);phw_uint(w,"max_peers",FEATURE_MAX_PEERS);}
 phw_end(w);free(c);return !w->failed;
}
static void feature_result_mp(ph_writer *w,int status,unsigned revision,const ph_value *p){
 phw_begin(w,NULL,false);phw_uint(w,"code",status);phw_uint(w,"revision",revision);
 phw_text(w,"message",status==200?"Saved":status==409?"Revision conflict":status>=500?"Persistence unavailable":"Invalid configuration");
 ph_value request;if(p&&phv_get(p,"request_id",&request))phw_value(w,"request_id",&request);phw_end(w);
}
int feature_domain_set_mp(const char *d,const ph_value *p,ph_writer *w){
 int status=200;
 if(!p||p->kind!=PHV_MAP){feature_result_mp(w,400,0,p);return 400;}
 if(!strcmp(d,"status")){feature_result_mp(w,405,0,p);return 405;}
#ifndef FEATURE_DEVICE_A1
 if(!strcmp(d,"binding")||!strcmp(d,"linkage")||!strcmp(d,"light")||!strcmp(d,"probe")){feature_result_mp(w,405,0,p);return 405;}
#endif
 feature_config_t *c=malloc(sizeof(*c));if(!c){feature_result_mp(w,500,0,p);return 500;}feature_config_get(c);
 ph_value revision;if(phv_get(p,"revision",&revision)){int64_t n;if(!phv_int(&revision,&n)||n<0||n>UINT32_MAX){status=400;goto done;}if((uint32_t)n!=c->revision){status=409;goto done;}}
#ifdef FEATURE_DEVICE_A1
 if(!strcmp(d,"probe")){
  char raw[18],mac[13];ph_value v;if(!phv_get(p,"mac",&v)||!phv_text(&v,raw,sizeof(raw))||!feature_mac(raw,mac)){status=400;goto done;}
  bool found=false;lock();for(unsigned i=0;i<c->peer_count;i++)if(!strcmp(c->peers[i].id,mac)){peers[i].recover_at=0;peers[i].sent=0;peers[i].failures=0;peers[i].pending=false;peers[i].lan=false;found=true;}unlock();
  if(!found)found=group_manual_probe(c,mac);if(!found)status=404;goto done;
 }
#endif
 if(!feature_config_decode_mp(c,d,p)){status=400;goto done;}
 if(!install_config(c)){
  /* A failed commit may race another writer. Distinguish its new revision
   * from allocation/Flash failure without publishing the uncommitted copy. */
  unsigned expected=c->revision;feature_config_get(c);status=c->revision!=expected?409:500;
 }else group_broadcast_config_mp(d,p);
 done:feature_result_mp(w,status,c->revision,p);free(c);return status;
}
bool feature_ntp_set_mp(const char *host,const char **code){
 if(!host||!host[0]||strlen(host)>=sizeof(((feature_config_t*)0)->ntp_host)||strpbrk(host,"\r\n")){*code="invalid_params";return false;}
 feature_config_t *c=malloc(sizeof(*c));if(!c){*code="busy";return false;}feature_config_get(c);strcpy(c->ntp_host,host);bool ok=install_config(c);free(c);*code=ok?"accepted":"storage_error";return ok;
}
bool feature_mp(const char *action,const ph_value *p,ph_writer *w,const char **code){
#ifdef FEATURE_DEVICE_A1
 if(!strcmp(action,"light_effect.next")||!strcmp(action,"light_effect.select")){
  bool next=!strcmp(action,"light_effect.next");ph_value effect;int64_t n=255;
  *code="invalid_params";
  if(!p||!phv_keys(p,next?"":"|effect|")||(!next&&(!phv_get(p,"effect",&effect)||!phv_int(&effect,&n)||n<0||n>5))){phw_begin(w,NULL,false);phw_end(w);return true;}
  int rc=feature_light_select_mp((unsigned)n,w);*code=rc<300?"accepted":rc==409?"conflict":rc>=500?"storage_error":"invalid_params";
  if(rc>=500||rc==400){phw_begin(w,NULL,false);phw_end(w);}return true;
 }
#endif
 const char *domain=NULL;bool set=false;
 if(!strcmp(action,"binding.status.get"))domain="status";
#ifdef FEATURE_DEVICE_A1
 else if(!strcmp(action,"sensors.average.get"))domain="average";
 else if(!strcmp(action,"binding.probe")){domain="probe";set=true;}
#endif
 const char *public[]={"binding","night","time","linkage","light_effect",NULL};
 const char *native[]={"binding","sleep","time","linkage","light",NULL};
 for(unsigned i=0;!domain&&public[i];i++){
#ifndef FEATURE_DEVICE_A1
  if(i>=3)break;
#endif
  char key[48];snprintf(key,sizeof(key),"%s.get",public[i]);if(!strcmp(action,key))domain=native[i];
  snprintf(key,sizeof(key),"%s.set",public[i]);if(!strcmp(action,key)){domain=native[i];set=true;}
 }
 if(!domain)return false;
 *code="invalid_params";
 if(!set){if(!p||!phv_keys(p,"")){phw_begin(w,NULL,false);phw_end(w);}else{*code="ok";if(!feature_domain_get_mp(domain,w)){*code="busy";/* Allocation failed before output. */if(!w->failed){phw_begin(w,NULL,false);phw_end(w);}}}}
 else{int rc=feature_domain_set_mp(domain,p,w);*code=rc<300?"accepted":rc==409?"conflict":rc>=500?"storage_error":"invalid_params";}
 return true;
}
