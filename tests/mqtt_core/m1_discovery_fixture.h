/* Test-only observer of bytes emitted by the real shared session/Paho. */
#include <assert.h>
#include "common/typed_wire.h"
static unsigned fixture_discoveries;
static void fixture_announcement(const unsigned char *topic,unsigned topic_size,
                                 const unsigned char *payload,size_t size,unsigned header)
{
    if(topic_size<23 || memcmp(topic+topic_size-23,"/discovery/",11))return;
    assert((header&1)&&((header>>1)&3)==1); /* retained QoS1 */
    ph_value body,v;int64_t n;
    assert(phv_parse(PH_WIRE_MSGPACK,payload,size,&body)&&body.kind==PHV_MAP);
    assert(phv_get(&body,"schema",&v)&&phv_int(&v,&n)&&n==2);
    assert(phv_get(&body,"protocol_version",&v)&&phv_int(&v,&n)&&n==2);
    assert(phv_get(&body,"wire_version",&v)&&phv_int(&v,&n)&&n==1);
    assert(phv_get(&body,"protocol",&v)&&phv_equal_text(&v,"tmcp"));
    assert(phv_get(&body,"model",&v)&&phv_equal_text(&v,"m1"));
    assert(phv_get(&body,"device_id",&v)&&v.kind==PHV_TEXT&&v.size==12&&!memcmp(v.text,topic+topic_size-12,12));
    assert(phv_get(&body,"firmware_version",&v)&&phv_equal_text(&v,"1.0.0-rc.4-test"));
    assert(phv_get(&body,"variant",&v)&&phv_equal_text(&v,"m1-test"));
    assert(phv_get(&body,"version",&v)&&phv_equal_text(&v,"v2"));
    assert(phv_get(&body,"default_wire_format",&v)&&phv_equal_text(&v,"msgpack"));
    assert(phv_get(&body,"wire_formats",&v)&&v.kind==PHV_ARRAY&&v.count==1);
    ph_value item;assert(phv_at(&v,0,&item)&&phv_equal_text(&item,"msgpack"));fixture_discoveries++;
}
