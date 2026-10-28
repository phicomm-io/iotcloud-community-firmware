/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The message metadata is copied; body is borrowed only for synchronous dispatch.
 * Retry slots own complete encoded packets, never request-buffer views. */
#ifdef FEATURE_DEVICE_A1
extern bool feature_control_mp(const char *,const ph_value *,const char **);
typedef struct {char src[13],dst[13],group[49],op[20],domain[16],kind[16];uint32_t boot,id;uint8_t token[16];ph_value body;} group_message;
static struct {char ip[16];uint32_t sent,retry_at,request,boot,last_id[11],retired_boot[4];unsigned failures,successes;bool lan,joined;ph_buf *pending;} group_members[8];
static bool group_token_empty(const uint8_t token[16]){unsigned bits=0;for(unsigned i=0;i<16;i++)bits|=token[i];return !bits;}
static bool group_text(const ph_value *root,const char *key,char *out,size_t cap){ph_value v;return phv_get(root,key,&v)&&phv_text(&v,out,cap);}
static ph_buf *group_envelope_mp(const feature_config_t *c,unsigned i,const char *op,uint32_t id,const char *domain,const char *kind,const ph_value *body)
{
 ph_writer w;if(!phw_init_capacity(&w,PH_WIRE_MSGPACK,256))return NULL;
 phw_begin(&w,NULL,false);phw_uint(&w,"v",3);phw_text(&w,"src",LOCAL_ID);phw_text(&w,"dst",c->members[i].id);phw_text(&w,"group",c->group);phw_text(&w,"op",op);phw_u32(&w,"boot",boot_id);phw_u32(&w,"id",id?id:next_request());phw_bytes_value(&w,"token",c->group_token,16);
 if(domain)phw_text(&w,"domain",domain);if(kind)phw_text(&w,"kind",kind);if(body)phw_value(&w,"body",body);phw_end(&w);return phw_take(&w);
}
static bool group_send_wire(const char *ip,const ph_buf *wire)
{
 if(!wire||wire->len>FEATURE_WIRE_MAX||udp<0||!ip||!ip[0])return false;
 struct sockaddr_in a={0};a.sin_family=AF_INET;a.sin_port=htons(FEATURE_PORT);a.sin_addr.s_addr=inet_addr(ip);
 if(!a.sin_addr.s_addr||a.sin_addr.s_addr==INADDR_NONE)return false;
 return sendto(udp,wire->buf,wire->len,0,(struct sockaddr*)&a,sizeof(a))==(int)wire->len;
}
static void group_mqtt_mp(unsigned i,const feature_config_t *c,const ph_buf *wire)
{
 if(!wire)return;char topic[FEATURE_TOPIC_MAX];mqtt_account_t account=mqtt_snapshot();
 if(!mqtt_link_topic(&account,"a1",c->members[i].id,LOCAL_ID,"request",topic,sizeof(topic)))return;
 char prefix[160];uint32_t generation;mico_rtos_lock_mutex(&namespace_lock);mqtt_format(prefix,sizeof(prefix),"phicomm/v2/%s/",feature_account.key);generation=mqtt_generation;mico_rtos_unlock_mutex(&namespace_lock);
 size_t n=strlen(prefix);if(!strncmp(topic,prefix,n)&&strlen(topic+n)<128)ph_mqtt_enqueue_send_generation(topic,wire->buf,wire->len,1,false,generation);
}
static void group_config_changed(const feature_config_t *old,const feature_config_t *next)
{
 if(!strcmp(old->group,next->group)&&old->member_count==next->member_count&&!memcmp(old->members,next->members,sizeof(old->members)))return;
 for(unsigned i=0;i<8;i++)ph_buf_free(group_members[i].pending);memset(group_members,0,sizeof(group_members));memset(member_seen,0,sizeof(member_seen));
}
static void group_send_mp(const feature_config_t *c,unsigned i,ph_buf *wire,uint32_t request)
{
 if(!wire)return;char ip[16];bool lan;lock();strcpy(ip,c->members[i].ip[0]?c->members[i].ip:group_members[i].ip);lan=group_members[i].lan&&!c->group_force_mqtt;unlock();
 ph_buf *copy=NULL;if(lan){copy=ph_buf_new(wire->len+1);if(copy){memcpy(copy->buf,wire->buf,wire->len);copy->len=wire->len;copy->buf[copy->len]=0;}}
 if(copy){lock();ph_buf *old=group_members[i].pending;group_members[i].pending=copy;group_members[i].request=request;group_members[i].sent=now_ms();unlock();if(old){group_mqtt_mp(i,c,old);ph_buf_free(old);}if(group_send_wire(ip,wire))return;lock();if(group_members[i].pending==copy){group_members[i].pending=NULL;}else copy=NULL;unlock();ph_buf_free(copy);}
 group_mqtt_mp(i,c,wire);
}
static bool group_input_mp(const ph_value *root,bool lan,bool retained,const char *ip)
{
 group_message m={0};if(!group_text(root,"op",m.op,sizeof(m.op))||strncmp(m.op,"group_",6))return false;if(retained)return true;
 uint32_t version;ph_value token;const void *bytes;size_t count;
 if(root->kind!=PHV_MAP||!phv_keys(root,"|v||src||dst||group||op||boot||id||token||domain||kind||body|")||!lan_u32(root,"v",&version)||version!=3||!lan_id(root,"src",m.src)||!lan_id(root,"dst",m.dst)||strcmp(m.dst,LOCAL_ID)||!group_text(root,"group",m.group,sizeof(m.group))||!lan_u32(root,"boot",&m.boot)||!m.boot||!lan_u32(root,"id",&m.id)||!m.id||!phv_get(root,"token",&token)||!phv_bytes(&token,&bytes,&count)||count!=16)return true;
 if(strcmp(m.op,"group_ack")&&strcmp(m.op,"group_config")&&strcmp(m.op,"group_control")&&strcmp(m.op,"group_probe")&&strcmp(m.op,"group_join"))return true;
 memcpy(m.token,bytes,16);group_text(root,"domain",m.domain,sizeof(m.domain));group_text(root,"kind",m.kind,sizeof(m.kind));phv_get(root,"body",&m.body);
 feature_config_t *c=malloc(sizeof(*c));if(!c)return true;feature_config_get(c);unsigned i;for(i=0;i<c->member_count;i++)if(!strcmp(c->members[i].id,m.src))break;
 if(i==c->member_count||!c->group[0]||strcmp(c->group,m.group)||!strcmp(m.src,LOCAL_ID))goto done;
 if(!strcmp(m.op,"group_join")&&!lan&&(strcmp(m.src,LOCAL_ID)<0||group_token_empty(c->group_token))){if(memcmp(c->group_token,m.token,16)){memcpy(c->group_token,m.token,16);if(!install_config(c))goto done;}}
 if(group_token_empty(c->group_token)||memcmp(c->group_token,m.token,16)){if(!lan||!feature_lan_auth(m.src,m.token,4u))goto done;}
 lock();member_seen[i]=now_ms();if(ip&&ip[0])snprintf(group_members[i].ip,sizeof(group_members[i].ip),"%s",ip);
 unsigned slot=10;const char *domains[]={"binding","linkage","light","schedule","sleep","time"};if(!strcmp(m.op,"group_config"))for(unsigned k=0;k<6;k++)if(!strcmp(m.domain,domains[k]))slot=k;
 if(!strcmp(m.op,"group_control"))slot=!strcmp(m.kind,"fan")?6:!strcmp(m.kind,"light")?7:!strcmp(m.kind,"child_lock")?8:9;
 bool retired=false;for(unsigned k=0;k<4;k++)retired|=group_members[i].retired_boot[k]&&group_members[i].retired_boot[k]==m.boot;
 if(retired){unlock();goto done;}if(group_members[i].boot!=m.boot){if(group_members[i].boot){memmove(group_members[i].retired_boot+1,group_members[i].retired_boot,3*sizeof(uint32_t));group_members[i].retired_boot[0]=group_members[i].boot;}group_members[i].boot=m.boot;memset(group_members[i].last_id,0,sizeof(group_members[i].last_id));}
 if(!strcmp(m.op,"group_ack")){group_members[i].joined=true;ph_buf *old=NULL;if(group_members[i].pending&&m.id==group_members[i].request){old=group_members[i].pending;group_members[i].pending=NULL;if(lan&&++group_members[i].successes>=2)group_members[i].lan=true;}unlock();ph_buf_free(old);goto done;}
 bool duplicate=(int32_t)(m.id-group_members[i].last_id[slot])<=0;unlock();
 bool ok=true;if(!duplicate){if(!strcmp(m.op,"group_control")){const char *code="invalid_params";ok=feature_control_mp(m.kind,&m.body,&code);}else if(!strcmp(m.op,"group_config"))ok=feature_config_decode_mp(c,m.domain,&m.body)&&install_config(c);else ok=!strcmp(m.op,"group_probe")||!strcmp(m.op,"group_join");}
 if(ok){if(!duplicate){lock();group_members[i].last_id[slot]=m.id;unlock();}ph_buf *reply=group_envelope_mp(c,i,"group_ack",m.id,NULL,NULL,NULL);if(lan)group_send_wire(ip,reply);else group_mqtt_mp(i,c,reply);ph_buf_free(reply);}
 done:free(c);return true;
}
static void group_poll(const feature_config_t *c,uint32_t now)
{
 if(!c->group[0])return;
 if(feature_is_coordinator()&&group_token_empty(c->group_token)){feature_config_t *next=malloc(sizeof(*next));if(next){*next=*c;new_token(next->group_token);install_config(next);free(next);}return;}
 for(unsigned i=0;i<c->member_count;i++){
  if(!strcmp(c->members[i].id,LOCAL_ID))continue;
  ph_buf *expired=NULL;char ip[16];bool joined,lan,pending;uint32_t retry,sent;
  lock();strcpy(ip,c->members[i].ip[0]?c->members[i].ip:group_members[i].ip);
  if(group_members[i].pending&&(uint32_t)(now-group_members[i].sent)>=c->group_timeout_ms){expired=group_members[i].pending;group_members[i].pending=NULL;group_members[i].lan=false;group_members[i].failures++;group_members[i].successes=0;group_members[i].retry_at=now+c->group_recovery_seconds*1000;}
  joined=group_members[i].joined;lan=group_members[i].lan&&!c->group_force_mqtt;pending=group_members[i].pending!=NULL;retry=group_members[i].retry_at;sent=group_members[i].sent;unlock();
  if(expired){group_mqtt_mp(i,c,expired);ph_buf_free(expired);}
  if(!joined&&feature_is_coordinator()&&(uint32_t)(now-sent)>=5000){ph_buf *wire=group_envelope_mp(c,i,"group_join",0,NULL,NULL,NULL);group_mqtt_mp(i,c,wire);ph_buf_free(wire);lock();group_members[i].sent=now;unlock();}
  if(joined&&!pending&&ip[0]&&!lan&&!c->group_force_mqtt&&(retry==0||c->group_recovery)&&(int32_t)(now-retry)>=0){
   uint32_t request=next_request();ph_buf *wire=group_envelope_mp(c,i,"group_probe",request,NULL,NULL,NULL);if(!wire)continue;
   /* Transfer ownership before send: a fast acknowledgement may run in sendto. */
   lock();bool installed=group_members[i].pending==NULL;if(installed){group_members[i].pending=wire;group_members[i].request=request;group_members[i].sent=now;}unlock();
   if(installed){/* Send a separate allocation: ACK may free the pending slot. */ph_buf *copy=group_envelope_mp(c,i,"group_probe",request,NULL,NULL,NULL);bool sent_ok=group_send_wire(ip,copy);ph_buf_free(copy);if(!sent_ok){lock();if(group_members[i].pending==wire){group_members[i].pending=NULL;}else wire=NULL;unlock();ph_buf_free(wire);}}
   else ph_buf_free(wire);
  }
 }
}
static bool group_manual_probe(const feature_config_t *c,const char *mac)
{
 for(unsigned i=0;i<c->member_count;i++)if(!strcmp(c->members[i].id,mac)){lock();ph_buf *old=group_members[i].pending;group_members[i].pending=NULL;group_members[i].retry_at=0;group_members[i].successes=0;group_members[i].lan=false;unlock();if(old){group_mqtt_mp(i,c,old);ph_buf_free(old);}return true;}return false;
}
static void group_broadcast_config_mp(const char *domain,const ph_value *body)
{
 feature_config_t *c=malloc(sizeof(*c));if(!c)return;feature_config_get(c);
 if(c->group[0])for(unsigned i=0;i<c->member_count;i++)if(strcmp(c->members[i].id,LOCAL_ID)){uint32_t id=next_request();ph_buf *wire=group_envelope_mp(c,i,"group_config",id,domain,NULL,body);group_send_mp(c,i,wire,id);ph_buf_free(wire);}free(c);
}
bool feature_group_control_mp(const char *kind,const ph_value *body)
{
 feature_config_t *c=malloc(sizeof(*c));if(!c)return false;feature_config_get(c);bool active=c->group[0];
 if(active)for(unsigned i=0;i<c->member_count;i++)if(strcmp(c->members[i].id,LOCAL_ID)){uint32_t id=next_request();ph_buf *wire=group_envelope_mp(c,i,"group_control",id,NULL,kind,body);group_send_mp(c,i,wire,id);ph_buf_free(wire);}free(c);return active;
}
#else
static void group_config_changed(const feature_config_t *a,const feature_config_t *b){(void)a;(void)b;}
static bool group_input_mp(const ph_value *v,bool lan,bool retained,const char *ip){(void)v;(void)lan;(void)retained;(void)ip;return false;}
static void group_poll(const feature_config_t *c,uint32_t now){(void)c;(void)now;}
static bool group_manual_probe(const feature_config_t *c,const char *mac){(void)c;(void)mac;return false;}
static void group_broadcast_config_mp(const char *domain,const ph_value *body){(void)domain;(void)body;}
bool feature_group_control_mp(const char *kind,const ph_value *body){(void)kind;(void)body;return false;}
#endif
