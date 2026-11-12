"""Keep the production Paho fixture intact and observe binary discovery bytes."""
def observe_discovery(source):
    marker = '  if(topic_size==15&&!memcmp(tx+offset+2,"tx/availability",15)){'
    assert source.count(marker) == 1
    probe = '  fixture_announcement(tx+offset+2,topic_size,tx+body_at,tx_size-body_at,tx[0]);\n'
    return '#include "tests/mqtt_core/m1_discovery_fixture.h"\n' + source.replace(marker, probe + marker)
