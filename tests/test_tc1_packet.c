#include "MQTTClient.h"
#include <assert.h>
#include <unistd.h>

int readPacket(Client *, Timer *);
static unsigned offset, acknowledged;
static bool publish;
static int delayed_read(Network *network, unsigned char *buffer, int length, int timeout)
{
    (void)network;
    const unsigned char ack[] = {0x40, 2, 0, 7};
    const unsigned char message[] = {0x32, 7, 0, 1, 'x', 0, 7, 'o', 'k'};
    const unsigned char *packet = publish ? message : ack;
    int delay = offset ? 50 : 90;
    if (timeout < delay) { if (timeout > 0) usleep(timeout * 1000); return 0; }
    usleep(delay * 1000);
    assert(offset + length <= (publish ? sizeof(message) : sizeof(ack)));
    memcpy(buffer, packet + offset, length); offset += length; return length;
}
static int acknowledge(Network *network, unsigned char *buffer, int length, int timeout)
{
    (void)network;
    assert(timeout > 0);
    assert(length == 4 && buffer[0] == 0x40 && buffer[3] == 7);
    ++acknowledged; return length;
}
int main(void)
{
    Network network = {0}; network.mqttread = delayed_read; network.mqttwrite = acknowledge;
    Client client = {0}; assert(MQTTClientInit(&client, &network, 500) == 0);
    Timer yield; InitTimer(&yield); countdown_ms(&yield, 100);
    assert(readPacket(&client, &yield) == PUBACK);
    assert(offset == 4 && client.readbuf[3] == 7);
    offset = 0; publish = true;
    assert(MQTTYield(&client, 100) == MQTT_SUCCESS);
    assert(offset == 9 && acknowledged == 1);
    MQTTClientDeinit(&client);
    puts("TC1 completes fragmented packets and ACKs beyond the idle deadline");
}
