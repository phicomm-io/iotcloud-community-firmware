/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "service.h"
#include "client.h"
#include "binary_busy.h"
#include "../ota/mode.h"
#include "mico.h"
#include "../a1_decimal.inc"
#include "publication.h"
static ph_mqtt_publication status_publication;
static bool snapshot_deferred,capabilities_pending;
static uint32_t snapshot_retry_at;
static int snapshot_wait(void){snapshot_deferred=true;snapshot_retry_at=mico_rtos_get_time();return PH_MQTT_MORE;}
void service_mqtt_clear(void){capabilities_pending=false;snapshot_deferred=false;memset(&status_publication,0,sizeof(status_publication));ph_mqtt_pending_clear();}
int service_mqtt_flush(service_mqtt_sender sender){(void)sender;return ph_mqtt_poll(NULL);}
static void receipt_release(void *wire){ph_buf_free(wire);}
int service_mqtt_input(service_mqtt_sender sender,const mqtt_account_t *scope,const void *payload,size_t length,bool retained)
{
    (void)sender;
    if(!ph_mqtt_owner() || !ph_mqtt_debug_core()->dispatch_depth)return MQTT_FAILURE;
    if(retained || (ota_job_active() && length>1024))return MQTT_SUCCESS;
    mqtt_target_t target;mqtt_target_set(&target,scope);
    char busy[SERVICE_BUSY_BYTES];size_t busy_length=mqtt_busy_wire(payload,length,busy);
    /* Execute while the ingress entry is borrowed. Returned bytes own every
     * value, including request_id; release the queue entry only afterwards. */
    ph_buf *wire=ph_mqtt_input_admit()?service_mqtt_command_mp(payload,length):NULL;
    bool consumed=ph_mqtt_input_consumed();
    if(!consumed){ph_buf_free(wire);wire=NULL;}
    return ph_mqtt_receipt_send_bytes(&target,wire?wire->buf:busy,wire?wire->len:busy_length,wire,receipt_release);
}
static int publish_query(service_mqtt_sender sender,const mqtt_account_t *scope,const char *action,const char *leaf)
{
    ph_buf *wire=service_query_mp(action);if(!wire)return snapshot_wait();
    char topic[MQTT_TOPIC_MAX];mqtt_leaf(scope,leaf,topic,sizeof(topic));
    int rc=sender(topic,wire->buf,wire->len,true);ph_buf_free(wire);return rc;
}
static int try_capabilities(service_mqtt_sender sender,const mqtt_account_t *scope)
{
    int rc=publish_query(sender,scope,"capabilities.get","capabilities");
    if(rc==MQTT_SUCCESS){capabilities_pending=false;snapshot_deferred=false;}return rc;
}
int service_mqtt_snapshot(service_mqtt_sender sender,const mqtt_account_t *scope)
{
    ph_mqtt_publication_attempt(&status_publication,mico_rtos_get_time());
    int ready=ph_mqtt_resource_ready();if(ready!=MQTT_SUCCESS)return ready==PH_MQTT_NOT_STARTED_RESOURCE_DEFERRED?PH_MQTT_MORE:ready;
    if(snapshot_deferred && (uint32_t)(mico_rtos_get_time()-snapshot_retry_at)<1000u)return PH_MQTT_MORE;
    if(capabilities_pending){int rc=try_capabilities(sender,scope);if(rc!=MQTT_SUCCESS)return rc;}
    if(ota_job_active())return snapshot_wait();
    int rc=publish_query(sender,scope,"status.get","state");if(rc==MQTT_SUCCESS){snapshot_deferred=false;ph_mqtt_publication_complete(&status_publication,mico_rtos_get_time());}return rc;
}
bool service_mqtt_status_due(bool changed)
{return ph_mqtt_publication_due(&status_publication,mico_rtos_get_time(),5000u,1000u,changed);}
int service_mqtt_capabilities(service_mqtt_sender sender,const mqtt_account_t *scope)
{capabilities_pending=true;memset(&status_publication,0,sizeof(status_publication));return try_capabilities(sender,scope);}
