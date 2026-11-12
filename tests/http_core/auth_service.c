/* Real dispatcher + HTTP response ownership, with deterministic gate waits. */
#include "../service_mp/fixture.inc"
#include "mico.h"
#include "common/httpd/product.h"

static ph_auth_state auth;
static char auth_password[33];
static pthread_mutex_t auth_lock=PTHREAD_MUTEX_INITIALIZER;
static bool fail_commit;
static uint32_t auth_random_value;
static uint32_t auth_now(void *u){(void)u;return test_clock_ms;}
static bool auth_random(void *u,uint8_t *p,size_t n){(void)u;memset(p,0,n);uint32_t v=++auth_random_value;memcpy(p,&v,sizeof(v));return true;}
static void auth_take(void *u){(void)u;assert(!pthread_mutex_lock(&auth_lock));}
static void auth_give(void *u){(void)u;assert(!pthread_mutex_unlock(&auth_lock));}
static bool auth_get_password(void *u,char out[33]){(void)u;strcpy(out,auth_password);return true;}
static bool auth_commit(void *u,const ph_auth_store *store){(void)store;if(fail_commit)return false;memmove(auth_password,u,strlen(u)+1);return true;}
static void credentials(const char *password,bool force){assert(ph_auth_credential_commit(&auth,password,force,auth_commit,(void *)password));}

/* sem_wait is the real host implementation behind service_command_enter(true).
 * Signal before entering it while the main thread owns the actual control gate;
 * never use sleeps to guess whether a revoke happened during the wait. */
static __thread bool http_worker;
static pthread_mutex_t waiting_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t waiting_changed=PTHREAD_COND_INITIALIZER;
static bool waiting_at_gate;
int __real_sem_wait(sem_t *);
int __real_sem_trywait(sem_t *);
int __wrap_sem_wait(sem_t *s){
 if(http_worker){
  int value;assert(!sem_getvalue(s,&value)&&value==0);
  assert(!pthread_mutex_lock(&waiting_lock));waiting_at_gate=true;
  assert(!pthread_cond_signal(&waiting_changed));assert(!pthread_mutex_unlock(&waiting_lock));
 }
 return __real_sem_wait(s);
}
int __wrap_sem_trywait(sem_t *s){assert(!http_worker);return __real_sem_trywait(s);}

enum { OPEN,BASIC,BEARER };
enum { COMMAND,RECEIPT,QUERY,CACHED_QUERY,COMMAND_QUERY };
enum { KEEP,NOOP,FAILED_RESTORE,RESTORE,CHANGE_BACK,REVOKE,LOGOUT,EXPIRE,MAX_AGE };
static ph_http_product product;
static ph_http_exchange exchange;
static ph_buf *input;
static unsigned operation,serial;
static char authorization[64];
static void capture(unsigned mode){
 ph_auth_store store;ph_auth_store_empty(&store);strcpy(auth_password,mode==OPEN?"":"pw");
 ph_auth_ops ops={.now=auth_now,.random=auth_random,.lock=auth_take,.unlock=auth_give,.password=auth_get_password};
 assert(ph_auth_init(&auth,&ops,&store));
 if(mode==BEARER){char token[33];assert(ph_auth_http_login(&auth,"pw",token));snprintf(authorization,sizeof(authorization),"Bearer %s",token);}
 else strcpy(authorization,mode==OPEN?"":"Basic YWRtaW46cHc=");
 assert(ph_auth_http_check(&auth,authorization,&product.auth));
 assert(ph_auth_context_valid(&product.auth,PH_AUTH_SCOPE_ADMIN));
}
static void dispatch(void){
 if(operation==QUERY||operation==CACHED_QUERY)ph_http_query(&product,&exchange,"status.get");
 else ph_http_command_mp(&product,&exchange,input->buf,input->len);
}
static void *dispatch_waiting(void *u){(void)u;http_worker=true;dispatch();http_worker=false;return NULL;}
static void assert_reply(unsigned status,const char *expected){
 assert(exchange.response.status==status&&product.wire);
 assert(exchange.response.body==product.wire->buf);code(product.wire,expected);
}
static void change_credentials(unsigned change){
 switch(change){
 case KEEP:break;
 case NOOP:credentials(auth_password,false);break;
 case FAILED_RESTORE:fail_commit=true;assert(!ph_auth_credential_commit(&auth,auth_password,true,auth_commit,auth_password));fail_commit=false;break;
 case RESTORE:credentials(auth_password,true);break;
 case CHANGE_BACK:{char original[33];strcpy(original,auth_password);credentials("other",false);credentials(original,false);break;}
 case REVOKE:ph_auth_local_revoke(&auth,&auth.store);break;
 case LOGOUT:assert(ph_auth_http_logout(&auth,authorization));break;
 case EXPIRE:test_clock_ms+=PH_AUTH_IDLE_MS;break;
 case MAX_AGE:test_clock_ms+=PH_AUTH_MAX_MS;break;
 default:abort();
 }
}
static void run_wait(unsigned mode,unsigned kind,unsigned change){
 ph_http_product_init(&product,&exchange);capture(mode);operation=kind;
 char id[64];snprintf(id,sizeof(id),"http-gate-%u",++serial);
 input=request(kind==COMMAND_QUERY?"status.get":"fan.set",id,-1);assert(input);
 if(kind==RECEIPT){dispatch();assert_reply(200,"accepted");}
 if(kind==CACHED_QUERY){dispatch();assert_reply(200,"ok");}
 if(kind==QUERY||kind==COMMAND_QUERY)service_cache_changed(SERVICE_CACHE_STATUS);
 unsigned before_mutations=mutations,before_captures=captures;
 assert(service_control_begin());
 assert(!pthread_mutex_lock(&waiting_lock));waiting_at_gate=false;
 pthread_t worker;assert(!pthread_create(&worker,NULL,dispatch_waiting,NULL));
 while(!waiting_at_gate)assert(!pthread_cond_wait(&waiting_changed,&waiting_lock));
 assert(!pthread_mutex_unlock(&waiting_lock));
 change_credentials(change);
 bool allowed=change==KEEP||change==NOOP||change==FAILED_RESTORE;
 assert(ph_auth_context_valid(&product.auth,PH_AUTH_SCOPE_ADMIN)==allowed);
 service_control_end();assert(!pthread_join(worker,NULL));
 assert_reply(allowed?200:401,allowed?(kind==COMMAND||kind==RECEIPT?"accepted":"ok"):"unauthorized");
 assert(mutations==before_mutations+(allowed&&kind==COMMAND));
 assert(captures==before_captures+(allowed&&(kind==QUERY||kind==COMMAND_QUERY)));
 ph_buf_free(input);input=NULL;ph_http_product_release(&product);
}
static void reject_foreign_contexts(void){
 ph_http_product_init(&product,&exchange);capture(OPEN);
 /* The longest valid request ID still maps a service auth error to HTTP 401. */
 char longest_id[65];memset(longest_id,'x',64);longest_id[64]=0;
 input=request("fan.set",longest_id,-1);
 ph_buf *out=service_http_command_mp(input->buf,input->len,NULL);code(out,"unauthorized");ph_buf_free(out);
 out=service_http_query_mp("status.get",NULL);code(out,"unauthorized");ph_buf_free(out);
 memset(&product.auth,0,sizeof(product.auth));operation=COMMAND;dispatch();assert_reply(401,"unauthorized");
 ph_auth_mqtt_context(&auth,&product.auth);assert(ph_auth_context_valid(&product.auth,PH_AUTH_SCOPE_ADMIN));dispatch();assert_reply(401,"unauthorized");
 ph_auth_internal_context(&auth,&product.auth);assert(ph_auth_context_valid(&product.auth,PH_AUTH_SCOPE_ADMIN));dispatch();assert_reply(401,"unauthorized");
 capture(OPEN);product.auth.scopes=PH_AUTH_SCOPE_CONTROL;dispatch();assert_reply(401,"unauthorized");
 ph_buf_free(input);ph_http_product_release(&product);
}
int main(void){
 service_init();
 for(unsigned mode=OPEN;mode<=BEARER;mode++)
  for(unsigned kind=COMMAND;kind<=COMMAND_QUERY;kind++)
   for(unsigned change=KEEP;change<=REVOKE;change++)run_wait(mode,kind,change);
 for(unsigned kind=COMMAND;kind<=COMMAND_QUERY;kind++)
  for(unsigned change=LOGOUT;change<=MAX_AGE;change++)run_wait(BEARER,kind,change);
 unsigned before=mutations;reject_foreign_contexts();assert(mutations==before);
 /* A refused call always releases the shared gate for the next command. */
 assert(service_control_begin());service_control_end();
 printf("HTTP service gate: %u blocked requests; open/Basic/Bearer, restore/revoke/logout/expiry, receipt/cache, failed/no-op commits, blocking policy and fail-closed source checks PASS\n",serial);
 return 0;
}
