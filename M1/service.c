#include "../common/service.c"
#include "../common/service_product.inc"
#include "../common/automation.c"

/* Only the trusted MQTT device command route grants temporary sessions. */
bool service_product_mqtt_mp(const char *action,const ph_value *params,ph_writer *w,const char **code){
 if(strcmp(action,"sensor.session.set"))return false;
 extern bool feature_sensor_session_mp(const ph_value *,const char **);
 feature_sensor_session_mp(params,code);phw_begin(w,NULL,false);phw_end(w);return true;
}
