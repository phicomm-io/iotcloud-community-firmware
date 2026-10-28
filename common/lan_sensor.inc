/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "lan_wire.inc"
/* Local password pairing is independent of broker-issued temporary grants. */
extern bool feature_lan_pair(const ph_value *,ph_writer *,const char *);
static bool group_text_source(const ph_value *root,char out[13]){return lan_id(root,"src",out);}
extern bool feature_lan_auth(const char *peer,const uint8_t token[16],unsigned scopes);
static bool lan_send(const char *ip,const lan_packet *packet)
{
  if(!packet||udp<0||!ip||!ip[0])return false;
  struct sockaddr_in a={0};a.sin_family=AF_INET;a.sin_port=htons(FEATURE_PORT);a.sin_addr.s_addr=inet_addr(ip);
  if(!a.sin_addr.s_addr||(a.sin_addr.s_addr==INADDR_NONE&&strcmp(ip,"255.255.255.255")))return false;
  char *storage=malloc(FEATURE_WIRE_MAX);if(!storage)return false;
  ph_buf buffer;ph_writer writer;
  bool ok=phw_init_fixed(&writer,PH_WIRE_MSGPACK,&buffer,storage,FEATURE_WIRE_MAX)&&lan_encode(&writer,packet);
  size_t n=ok?phw_finish_fixed(&writer):0;
  bool sent=n&&sendto(udp,storage,n,0,(struct sockaddr*)&a,sizeof(a))==(int)n;
  free(storage);return sent;
}
#ifndef FEATURE_DEVICE_A1
static void lan_sample_capture(lan_sample *out,const feature_sample_t *sample,uint32_t now)
{
  static const double scales[]={1000,1000,1000,1000000,1};
  memset(out,0,sizeof(*out));out->boot=boot_id;out->sequence=sample->sequence;
  out->co2_supported=sample->co2_supported;
  unsigned fresh=feature_sample_fresh_fields(sample,now);
  for(unsigned i=0;i<5;i++){
    bool valid=i==4?feature_co2_fresh(sample,now):(fresh&(1u<<i))!=0;
    uint32_t elapsed=now-(i==4?sample->co2_received_ms:sample->received_ms);
    uint32_t age=i==4?sample->co2_age_ms:sample->partial?sample->field_age_ms[i]:sample->age_ms;
    out->age_ms[i]=UINT32_MAX-age<elapsed?UINT32_MAX:age+elapsed;
    double value=(i==4?sample->co2:sample->value[i])*scales[i];
    if(valid&&isfinite(value)&&value>=INT32_MIN&&value<=INT32_MAX){out->fields|=1u<<i;out->value[i]=(int32_t)(value+(value>=0?.5:-.5));}
  }
}
#endif
static void lan_sensor_input(const lan_packet *packet,const char *ip)
{
  if(strcmp(packet->dst,LOCAL_ID)||!ip||!ip[0])return;
#ifdef FEATURE_DEVICE_A1
  if(packet->op!=LAN_SAMPLE&&packet->op!=LAN_PONG)return;
  feature_config_t *c=malloc(sizeof(*c));if(!c)return;feature_config_get(c);
  unsigned i;for(i=0;i<c->peer_count;i++)if(!strcmp(c->peers[i].id,packet->src))break;
  if(i==c->peer_count){free(c);return;}
  lock();
  bool matched=peer_current(c,i)&&peers[i].pending&&packet->id==peers[i].request&&!memcmp(peers[i].session_token,packet->token,16);
  if(matched){peers[i].pending=false;peers[i].lan=peers[i].authorized=true;peers[i].failures=0;peers[i].last_seen=now_ms();snprintf(peers[i].ip,sizeof(peers[i].ip),"%s",ip);}
  if(matched&&packet->op==LAN_SAMPLE){
    const lan_sample *wire=&packet->sample;bool retired=false;
    for(unsigned k=0;k<4;k++)retired|=peers[i].retired_boot[k]&&peers[i].retired_boot[k]==wire->boot;
    if(!retired&&(wire->boot!=samples[i].boot||(int32_t)(wire->sequence-samples[i].sequence)>0)){
      if(samples[i].boot&&wire->boot!=samples[i].boot){memmove(peers[i].retired_boot+1,peers[i].retired_boot,3*sizeof(uint32_t));peers[i].retired_boot[0]=samples[i].boot;}
      feature_sample_t sample={0};sample.partial=true;sample.boot=wire->boot;sample.sequence=wire->sequence;sample.received_ms=sample.co2_received_ms=now_ms();
      static const float scales[]={.001f,.001f,.001f,.000001f};
      for(unsigned f=0;f<4;f++)if((wire->fields&(1u<<f))&&wire->age_ms[f]<FEATURE_STALE_MS){sample.fields|=1u<<f;sample.value[f]=wire->value[f]*scales[f];sample.field_age_ms[f]=wire->age_ms[f];if(wire->age_ms[f]>sample.age_ms)sample.age_ms=wire->age_ms[f];}
      sample.valid=sample.fields!=0;sample.co2_supported=wire->co2_supported;sample.co2_valid=(wire->fields&16)&&wire->age_ms[4]<20000;sample.co2=wire->value[4];sample.co2_age_ms=wire->age_ms[4];samples[i]=sample;
    }
  }
  unlock();free(c);
#else
  if(packet->op!=LAN_READ&&packet->op!=LAN_PROBE)return;
  uint32_t now=now_ms();bool permitted=false;
  lock();
  for(unsigned i=0;i<FEATURE_MAX_PEERS;i++){
    sensor_session_t *session=&sensor_sessions[i];
    if(session->valid&&(uint32_t)(now-session->last_seen)>=SENSOR_SESSION_IDLE_MS)session->valid=false;
    if(session->valid&&!strcmp(session->id,packet->src)&&session->boot==packet->boot&&!memcmp(session->token,packet->token,16)){
      session->last_seen=session->last_read=now;session->active=true;snprintf(session->ip,sizeof(session->ip),"%s",ip);permitted=true;break;
    }
  }
  feature_sample_t sample=local_sample;unlock();
  if(!permitted)permitted=feature_lan_auth(packet->src,packet->token,2u);
  if(!permitted)return;
  lan_packet reply={0};reply.op=packet->op==LAN_READ?LAN_SAMPLE:LAN_PONG;strcpy(reply.src,LOCAL_ID);strcpy(reply.dst,packet->src);reply.boot=boot_id;reply.id=packet->id;memcpy(reply.token,packet->token,16);
  if(reply.op==LAN_SAMPLE)lan_sample_capture(&reply.sample,&sample,now);
  lan_send(ip,&reply);
#endif
}
