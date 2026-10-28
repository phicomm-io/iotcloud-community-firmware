/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "local_auth.h"
#include "lan_client.h"
#include "lan_client_core.inc"
static lc_state lan_clients;
static mico_mutex_t lan_client_lock;
static uint32_t lan_client_epoch(void){return device_local_auth()?ph_auth_credential_epoch(device_local_auth()):0;}
static bool lan_client_load(const char *peer,char key[33],unsigned *scopes){return device_local_auth()&&ph_auth_remote_get(device_local_auth(),peer,key,scopes);}
static bool lan_client_save(const char *peer,const char *key,unsigned scopes,uint32_t epoch){return device_local_auth()&&ph_auth_remote_store(device_local_auth(),peer,key,scopes,epoch);}
static bool lan_client_send(const char *ip,const void *data,size_t n)
{
 if(udp<0||!ip||!ip[0]||n>FEATURE_WIRE_MAX)return false;
 struct sockaddr_in address={0};address.sin_family=AF_INET;address.sin_port=htons(FEATURE_PORT);address.sin_addr.s_addr=inet_addr(ip);
 if(!address.sin_addr.s_addr||address.sin_addr.s_addr==INADDR_NONE)return false;
 return sendto(udp,data,n,0,(struct sockaddr*)&address,sizeof(address))==(int)n;
}
static lc_ops lan_client_ops={LOCAL_ID,now_ms,lan_client_epoch,new_token,lan_client_load,lan_client_save,lan_client_send};
bool feature_lan_pair_start(const char *peer,const char *ip,const char *password,unsigned scopes,const char **code)
{
 if(!code)return false;
 if(!device_local_auth()){*code="busy";return false;}
 if(!ip||!inet_addr(ip)||inet_addr(ip)==INADDR_NONE){*code="invalid_params";return false;}
 mico_rtos_lock_mutex(&lan_client_lock);bool ok=lc_start(&lan_clients,&lan_client_ops,peer,ip,password,scopes,code);mico_rtos_unlock_mutex(&lan_client_lock);return ok;
}
bool feature_lan_pair_status(const char *peer,ph_writer *writer)
{
 static const char *phases[]={"idle","pairing","resuming","ready","failed"};
 if(!lc_peer(peer))return false;
 mico_rtos_lock_mutex(&lan_client_lock);lc_job *job=lc_find(&lan_clients,peer,false);
 bool found=job!=NULL;if(found){phw_begin(writer,NULL,false);phw_text(writer,"phase",phases[job->phase]);phw_text(writer,"code",job->code?job->code:"pending");phw_end(writer);}
 mico_rtos_unlock_mutex(&lan_client_lock);return found;
}
static bool lan_client_token(const char *peer,const char *ip,unsigned scope,uint8_t out[16])
{
 if(!ip||!ip[0])return false;
 mico_rtos_lock_mutex(&lan_client_lock);bool ready=lc_token(&lan_clients,&lan_client_ops,peer,ip,scope,out);mico_rtos_unlock_mutex(&lan_client_lock);return ready;
}
static void lan_client_invalidate(const char *peer)
{
 mico_rtos_lock_mutex(&lan_client_lock);lc_job *job=lc_find(&lan_clients,peer,false);if(job&&job->phase==LC_READY){memset(job->token,0,sizeof(job->token));lc_resume(job,&lan_client_ops);}mico_rtos_unlock_mutex(&lan_client_lock);
}
static bool lan_client_response(const ph_value *root,const char *ip)
{
 mico_rtos_lock_mutex(&lan_client_lock);bool handled=lc_response(&lan_clients,&lan_client_ops,root,ip);mico_rtos_unlock_mutex(&lan_client_lock);return handled;
}
static void lan_client_poll(void)
{
 mico_rtos_lock_mutex(&lan_client_lock);lc_tick(&lan_clients,&lan_client_ops);mico_rtos_unlock_mutex(&lan_client_lock);
}
