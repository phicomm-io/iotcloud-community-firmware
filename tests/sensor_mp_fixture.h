/* Host-only observation of actual schema3 packets. Metadata is validated as a
 * separate retained publication; scenario counters count only state attempts.
 * No JSON codec or production algorithm is substituted. */
#include "common/sensor_mp.h"
static unsigned fixture_metadata;
static bool fixture_sensor_output(const char*topic,const void*bytes,size_t n,bool retained,const char*field,const char*name,const char*unit,int result){
 ph_value root,v;ph_sensor_view view;assert(phv_parse(1,bytes,n,&root));
 assert(ph_sensor_view_parse(&root,"aabbccddeeff",retained,&view));
 if(retained){assert(strstr(topic,"/metadata"));assert(ph_sensor_descriptor_matches(&view,field,unit));ph_value d;assert(phv_get(&view.fields,field,&d)&&phv_get(&d,"name",&v)&&phv_equal_text(&v,name));fixture_metadata++;return true;}
 assert(fixture_metadata&&strstr(topic,"/state"));if(result)return false;
 ph_value reading;bool valid;int64_t age;double value=0;
 assert(phv_get(&view.fields,field,&reading));assert(phv_get(&reading,"valid",&v)&&phv_bool(&v,&valid));assert(phv_get(&reading,"age_ms",&v)&&phv_int(&v,&age));assert(phv_get(&reading,"value",&v));if(valid)assert(phv_number(&v,&value));else assert(v.kind==PHV_NULL);
 printf("PUB\t0\t%s\t{\"schema\":3,\"device_id\":\"aabbccddeeff\",\"boot\":%u,\"sequence\":%u,\"descriptor_revision\":%u,\"sensors\":{\"%s\":{\"type\":\"number\",\"name\":\"%s\",\"unit\":\"%s\",\"valid\":%s,\"age_ms\":%lld,\"value\":",topic,view.boot,view.sequence,view.descriptor_revision,field,name,unit,valid?"true":"false",(long long)age);
 if(valid)printf("%.17g",value);else fputs("null",stdout);puts("}}}");return false;
}
