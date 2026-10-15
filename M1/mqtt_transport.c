#include "../common/mqtt_transport.h"

int m1_mqtt_read(Network *network, unsigned char *buffer, int length, int timeout_ms)
{
    return mqtt_socket_read(network, buffer, length, timeout_ms);
}
