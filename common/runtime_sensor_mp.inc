/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Included by runtime.c after namespace/config helpers; no borrowed view escapes. */
#include "sensor_mp.h"
static const char *const sensor_mp_names[]={"temperature","humidity","pm25","formaldehyde","co2"};
static const char *const sensor_mp_units[]={"°C","%","µg/m³","mg/m³","ppm"};
typedef struct { char id[13];uint32_t boot,revision,retired[4];unsigned fields; } sensor_descriptor_cache;
#ifdef FEATURE_DEVICE_A1
static sensor_descriptor_cache sensor_descriptors[FEATURE_MAX_PEERS];
#else
static sensor_descriptor_cache sensor_descriptors[1];
#endif
static void sensor_descriptors_reset(void){memset(sensor_descriptors,0,sizeof(sensor_descriptors));}
static bool sensor_descriptor_update(sensor_descriptor_cache *c,const char *id,const ph_sensor_view *v,bool *changed)
{
    sensor_descriptor_cache previous=*c;*changed=false;
    if(strcmp(c->id,id)){memset(c,0,sizeof(*c));strcpy(c->id,id);}
    for(unsigned i=0;i<4;i++)if(c->retired[i]==v->boot)return false;
    if(c->boot==v->boot&&(int32_t)(v->descriptor_revision-c->revision)<0)return false;
    if(c->boot&&c->boot!=v->boot){memmove(c->retired+1,c->retired,3*sizeof(*c->retired));c->retired[0]=c->boot;}
    c->boot=v->boot;c->revision=v->descriptor_revision;c->fields=0;
    for(unsigned f=0;f<5;f++)if(ph_sensor_descriptor_matches(v,sensor_mp_names[f],sensor_mp_units[f]))c->fields|=1u<<f;
    *changed=memcmp(&previous,c,sizeof(*c))!=0;return true;
}
static void sensor_sample_decode_mp(const ph_sensor_view *v,const sensor_descriptor_cache *c,feature_sample_t *s)
{
    memset(s,0,sizeof(*s));s->partial=true;s->received_ms=s->co2_received_ms=now_ms();s->boot=v->boot;s->sequence=v->sequence;
    s->co2_supported=(c->fields&16u)!=0;
    for(unsigned f=0;f<5;f++){
        float value=0;uint32_t age=0;
        if(!(c->fields&(1u<<f))||!ph_sensor_reading(v,sensor_mp_names[f],f==4?20000:FEATURE_STALE_MS,&value,&age)||
           (f==0&&(value< -40||value>125))||(f==1&&(value<0||value>100))||(f>=2&&value<0)||(f==4&&(value<1||value>65535)))continue;
        if(f==4){s->co2=value;s->co2_valid=true;s->co2_age_ms=age;}
        else{s->fields|=1u<<f;s->value[f]=value;s->field_age_ms[f]=age;if(age>s->age_ms)s->age_ms=age;}
    }s->valid=s->fields!=0;
}
static bool runtime_sensor_input_mp(const mqtt_account_t *account,const char *topic,const void *data,size_t n,bool retained)
{
    char prefix[FEATURE_TOPIC_MAX];if(!mqtt_aux(account,"sensor","",prefix,sizeof(prefix)))return false;size_t length=strlen(prefix);
    if(strncmp(topic,prefix,length))return false;
    const char *suffix=topic+length;char id[13];size_t size=strlen(suffix);bool metadata=size==21&&!strcmp(suffix+12,"/metadata");
    if(!metadata&&(size!=18||strcmp(suffix+12,"/state")))return false;
    memcpy(id,suffix,12);id[12]=0;char normalized[13];if(!feature_mac(id,normalized)||strcmp(normalized,id))return true;
    if(!metadata&&retained)return true;
    ph_value body;ph_sensor_view view;if(!phv_parse(PH_WIRE_MSGPACK,data,n,&body)||!ph_sensor_view_parse(&body,id,metadata,&view))return true;
#ifndef FEATURE_DEVICE_A1
    if(metadata)m1_display_source_metadata(id,&body);else m1_display_source_sample(id,&body);
    m1_co2_source_config_t config;m1_co2_source_config_get(&config);if(!config.external||strcmp(config.device_id,id))return true;
    sensor_descriptor_cache *c=&sensor_descriptors[0];
    if(metadata){bool changed;if(sensor_descriptor_update(c,id,&view,&changed)&&changed)m1_co2_source_invalidate(id);return true;}
    for(unsigned j=0;j<4;j++)if(c->retired[j]==view.boot)return true;
    if(strcmp(c->id,id)||c->boot!=view.boot||c->revision!=view.descriptor_revision){m1_co2_source_invalidate(id);return true;}
    feature_sample_t sample;sensor_sample_decode_mp(&view,c,&sample);m1_co2_source_sample(id,&sample);
#else
    lock();for(unsigned i=0;i<configuration.peer_count;i++)if(!strcmp(configuration.peers[i].id,id)){
        sensor_descriptor_cache *c=&sensor_descriptors[i];
        bool lan_fresh=peers[i].lan&&(uint32_t)(now_ms()-peers[i].last_seen)<FEATURE_STALE_MS;
        if(metadata){bool changed;if(sensor_descriptor_update(c,id,&view,&changed)&&changed&&!lan_fresh){samples[i].valid=false;samples[i].fields=0;samples[i].co2_valid=false;}break;}
        if(lan_fresh)break;
        bool retired_descriptor=false;for(unsigned j=0;j<4;j++)retired_descriptor|=c->retired[j]==view.boot;
        if(retired_descriptor)break;
        if(strcmp(c->id,id)||c->boot!=view.boot||c->revision!=view.descriptor_revision){samples[i].valid=false;samples[i].fields=0;samples[i].co2_valid=false;break;}
        feature_sample_t sample;sensor_sample_decode_mp(&view,c,&sample);
        bool retired=false;for(unsigned k=0;k<4;k++)retired|=peers[i].retired_boot[k]==sample.boot;
        if(retired||(samples[i].partial&&sample.boot==samples[i].boot&&(int32_t)(sample.sequence-samples[i].sequence)<=0))break;
        if(samples[i].partial&&samples[i].boot!=sample.boot){memmove(peers[i].retired_boot+1,peers[i].retired_boot,3*sizeof(uint32_t));peers[i].retired_boot[0]=samples[i].boot;}
        samples[i]=sample;break;
    }unlock();
    service_cache_changed(SERVICE_CACHE_STATUS);
#endif
    return true;
}
