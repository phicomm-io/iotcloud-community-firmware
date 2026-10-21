/* Fake product boundary for dispatcher-only host fixtures. The actual model
 * capture/writer is covered by test_status_direct on each pinned SDK ABI. */
#include "../../common/service.h"
#include "../../common/json_owned.h"
struct printbuf *service_product_status_wire(bool live,bool envelope,const char *id)
{
    json_object *p=json_object_new_object();
    if(live)json_build_object_add(p,"view",json_object_new_string("live"));
    const char *code="ok";json_object *out=service_product("status.get",p,&code);json_object_put(p);
    if(!out || strcmp(code,"ok") || !json_object_object_get(out,"model")) { json_object_put(out);return NULL; }
    if(envelope) {
        json_object *root=json_object_new_object();
        if(id)json_build_object_add(root,"request_id",json_object_new_string(id));
        json_build_object_add(root,"code",json_object_new_string(code));
        json_build_object_add(root,"data",out);out=root;
    }
    return json_owned_to_wire(out);
}
