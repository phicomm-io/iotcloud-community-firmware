#include "MQTTClient.h"

/* The moc SDK socket has a five-second receive timeout. Poll first so MQTT's
 * shorter yield deadline is honored and an idle connection is not an error. */
int m1_mqtt_read(Network *network, unsigned char *buffer, int length, int timeout_ms)
{
    Timer deadline;
    InitTimer(&deadline);
    countdown_ms(&deadline, timeout_ms > 0 ? timeout_ms : 0);
    int received = 0;
    while (received < length) {
        int remaining = left_ms(&deadline);
        if (remaining <= 0) return received;
        fd_set reads;
        FD_ZERO(&reads); FD_SET(network->my_socket, &reads);
        struct timeval wait = { remaining / 1000, (remaining % 1000) * 1000 };
        int ready = select(network->my_socket + 1, &reads, NULL, NULL, &wait);
        if (!ready) return received;
        if (ready < 0) return -1;
        int count = recv(network->my_socket, buffer + received, length - received, 0);
        if (count <= 0) return -1;
        received += count;
    }
    return received;
}
