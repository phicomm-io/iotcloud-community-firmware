#include "../common/service.c"
#include "../common/service_product.inc"
#include "../common/automation.c"

/* Session authorization is available only on the device MQTT command topic. */
json_object *service_product_mqtt(const char *action,json_object *params,const char **code) {
    *code="unsupported";
    if(strcmp(action,"sensor.session.set"))return NULL;
    int rc=feature_sensor_session_set(params);
    *code=rc==200?"accepted":rc==409?"busy":"invalid_params";
    return NULL;
}
