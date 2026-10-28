/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Portable sender state machine. Caller serializes access with its own mutex.
 * No borrowed views survive lc_response. Core persistence owns its own locks. */
#define LC_SLOTS 12
#define LC_PAIR 1
#define LC_RESUME 2
#define LC_READY 3
#define LC_FAILED 4
typedef struct {
 char peer[13],ip[16],password[33];uint8_t nonce[16],token[16];
 unsigned scopes,phase,attempts;uint32_t epoch,sent,ready;
 const char *code;
} lc_job;
typedef struct {lc_job jobs[LC_SLOTS];} lc_state;
typedef struct {
 const char *local;
 uint32_t (*now)(void),(*epoch)(void);
 bool (*random)(uint8_t[16]);
 bool (*load)(const char *,char[33],unsigned *);
 bool (*save)(const char *,const char *,unsigned,uint32_t);
 bool (*send)(const char *,const void *,size_t);
} lc_ops;
static lc_job *lc_find(lc_state *state,const char *peer,bool create)
{
 lc_job *empty=NULL;
 for(unsigned i=0;i<LC_SLOTS;i++){lc_job *job=&state->jobs[i];if(!strcmp(job->peer,peer))return job;if(!job->peer[0]&&!empty)empty=job;}
 if(create&&empty){strcpy(empty->peer,peer);return empty;}return NULL;
}
static void lc_fail(lc_job *job,const char *code)
{
 memset(job->password,0,sizeof(job->password));memset(job->nonce,0,sizeof(job->nonce));memset(job->token,0,sizeof(job->token));job->phase=LC_FAILED;job->code=code;
}
static bool lc_peer(const char *peer)
{
 if(!peer||strlen(peer)!=12)return false;
 for(unsigned i=0;i<12;i++)if(!((peer[i]>='0'&&peer[i]<='9')||(peer[i]>='a'&&peer[i]<='f')))return false;return true;
}
static bool lc_start(lc_state *state,const lc_ops *ops,const char *peer,const char *ip,const char *password,unsigned scopes,const char **code)
{
 *code="invalid_params";
 if(!lc_peer(peer)||!strcmp(peer,ops->local)||!ip||!ip[0]||strlen(ip)>15||!password||strlen(password)>32||!scopes||(scopes&~7u))return false;
 lc_job *job=lc_find(state,peer,true);if(!job){*code="busy";return false;}
 if(job->phase==LC_PAIR||job->phase==LC_RESUME){*code="busy";return false;}
 memset(job,0,sizeof(*job));strcpy(job->peer,peer);strcpy(job->ip,ip);strcpy(job->password,password);job->scopes=scopes;job->epoch=ops->epoch();
 if(!ops->random(job->nonce)){lc_fail(job,"busy");*code="busy";return false;}
 job->phase=LC_PAIR;job->code="pending";*code="accepted";return true;
}
static bool lc_resume(lc_job *job,const lc_ops *ops)
{
 char key[33]={0};unsigned scopes=0;bool exists=ops->load(job->peer,key,&scopes);memset(key,0,sizeof(key));
 if(!exists){lc_fail(job,"not_paired");return false;}
 job->scopes=scopes;job->epoch=ops->epoch();job->attempts=0;job->sent=0;job->phase=LC_RESUME;job->code="pending";
 if(!ops->random(job->nonce)){lc_fail(job,"busy");return false;}return true;
}
static bool lc_token(lc_state *state,const lc_ops *ops,const char *peer,const char *ip,unsigned scope,uint8_t token[16])
{
 /* Only callers with trusted configuration/learned addressing may select IP.
  * A new address starts a fresh exchange; packets from the old address cannot
  * complete it, even when they carry the previous matching nonce. */
 if(!ip||!ip[0]||strlen(ip)>15)return false;
 lc_job *job=lc_find(state,peer,false);
 if(!job){char key[33]={0};unsigned scopes;bool exists=ops->load(peer,key,&scopes);memset(key,0,sizeof(key));if(!exists||!(scopes&scope))return false;job=lc_find(state,peer,true);if(!job)return false;strcpy(job->ip,ip);if(!lc_resume(job,ops))return false;}
 if(strcmp(job->ip,ip)){
  strcpy(job->ip,ip);
  memset(job->password,0,sizeof(job->password));
  memset(job->token,0,sizeof(job->token));
  memset(job->nonce,0,sizeof(job->nonce));
  if(!lc_resume(job,ops))return false;
 }
 if(job->phase==LC_FAILED&&(uint32_t)(ops->now()-job->sent)>=60000u)lc_resume(job,ops);
 if(job->phase==LC_READY&&job->epoch!=ops->epoch()){memset(job->token,0,sizeof(job->token));lc_resume(job,ops);}
 if(job->phase==LC_READY&&(uint32_t)(ops->now()-job->ready)>=1200000u)lc_resume(job,ops);
 if(job->phase!=LC_READY||!(job->scopes&scope))return false;
 memcpy(token,job->token,16);return true;
}
static void lc_tick(lc_state *state,const lc_ops *ops)
{
 for(unsigned i=0;i<LC_SLOTS;i++){
  lc_job *job=&state->jobs[i];if(job->phase!=LC_PAIR&&job->phase!=LC_RESUME)continue;
  uint32_t now=ops->now();if(job->epoch!=ops->epoch()){lc_fail(job,"auth_required");continue;}
  if(job->attempts&&(uint32_t)(now-job->sent)<1000)continue;
  if(job->attempts>=3){lc_fail(job,"timeout");continue;}
  char storage[256],keyhex[33]={0};uint8_t key[16];ph_buf buf;ph_writer writer;
  if(!phw_init_fixed(&writer,PH_WIRE_MSGPACK,&buf,storage,sizeof(storage))){lc_fail(job,"busy");continue;}
  phw_begin(&writer,NULL,false);phw_text(&writer,"op",job->phase==LC_PAIR?"auth_pair":"auth_resume");phw_text(&writer,"src",ops->local);phw_bytes_value(&writer,"nonce",job->nonce,16);
  if(job->phase==LC_PAIR){phw_text(&writer,"password",job->password);phw_uint(&writer,"scopes",job->scopes);}
  else {unsigned scopes;if(!ops->load(job->peer,keyhex,&scopes)||!ph_auth_token_decode(keyhex,key)){memset(keyhex,0,sizeof(keyhex));lc_fail(job,"not_paired");continue;}phw_bytes_value(&writer,"key",key,16);memset(key,0,sizeof(key));memset(keyhex,0,sizeof(keyhex));}
  phw_end(&writer);size_t length=phw_finish_fixed(&writer);job->sent=now;job->attempts++;
  if(length)ops->send(job->ip,storage,length);memset(storage,0,sizeof(storage));
 }
}
static bool lc_response(lc_state *state,const lc_ops *ops,const ph_value *root,const char *ip)
{
 ph_value v;char peer[13],operation[20],code[32];const void *nonce;size_t n;
 if(!phv_get(root,"code",&v))return false;
 if(!phv_get(root,"src",&v)||!phv_text(&v,peer,sizeof(peer))||!phv_get(root,"op",&v)||!phv_text(&v,operation,sizeof(operation))||!phv_get(root,"nonce",&v)||!phv_bytes(&v,&nonce,&n)||n!=16)return true;
 lc_job *job=lc_find(state,peer,false);if(!job||(job->phase!=LC_PAIR&&job->phase!=LC_RESUME)||strcmp(ip,job->ip)||memcmp(nonce,job->nonce,16)||strcmp(operation,job->phase==LC_PAIR?"auth_pair":"auth_resume"))return true;
 if(!phv_get(root,"code",&v)||!phv_text(&v,code,sizeof(code)))return true;
 if(strcmp(code,"ok")){if(!strcmp(code,"busy"))return true;lc_fail(job,!strcmp(code,"storage_error")?"storage_error":"auth_required");return true;}
 const void *secret;if(!phv_get(root,job->phase==LC_PAIR?"key":"token",&v)||!phv_bytes(&v,&secret,&n)||n!=16)return true;
 unsigned bits=0;for(unsigned i=0;i<16;i++)bits|=((const uint8_t*)secret)[i];if(!bits)return true;
 if(job->epoch!=ops->epoch()){lc_fail(job,"auth_required");return true;}
 if(job->phase==LC_PAIR){char hex[33];static const char digits[]="0123456789abcdef";for(unsigned i=0;i<16;i++){hex[2*i]=digits[((const uint8_t*)secret)[i]>>4];hex[2*i+1]=digits[((const uint8_t*)secret)[i]&15];}hex[32]=0;
  bool saved=ops->save(job->peer,hex,job->scopes,job->epoch);memset(hex,0,sizeof(hex));memset(job->password,0,sizeof(job->password));if(!saved){lc_fail(job,"storage_error");return true;}lc_resume(job,ops);
 }else {memcpy(job->token,secret,16);job->phase=LC_READY;job->ready=ops->now();job->code="ok";memset(job->nonce,0,sizeof(job->nonce));}
 return true;
}
