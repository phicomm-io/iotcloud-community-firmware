#include "MQTTClient.h"
#include <assert.h>
#include <unistd.h>

int readPacket(Client *, Timer *);
static unsigned offset;
static int delayed_read(Network *network, unsigned char *buffer, int length, int timeout)
{
    (void)network;
    const unsigned char packet[] = {0x40, 2, 0, 7};
    int delay = offset ? 50 : 90;
    if (timeout < delay) { usleep(timeout * 1000); return 0; }
    usleep(delay * 1000);
    assert(offset + length <= sizeof(packet));
    memcpy(buffer, packet + offset, length); offset += length; return length;
}
int main(void)
{
    Network network = {0}; network.mqttread = delayed_read;
    Client client = {0}; assert(MQTTClientInit(&client, &network, 500) == 0);
    Timer yield; InitTimer(&yield); countdown_ms(&yield, 100);
    assert(readPacket(&client, &yield) == PUBACK);
    assert(offset == 4);
    assert(client.readbuf[3] == 7);
    MQTTClientDeinit(&client);
    puts("M1 completes fragmented MQTT packets beyond the idle yield deadline");
}
