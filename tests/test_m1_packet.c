#include "MQTTClient.h"
#include <assert.h>
#include <unistd.h>

int readPacket(Client *, Timer *);
int cycle(Client *, Timer *);
static const unsigned char *incoming;
static unsigned incoming_length, acknowledgements;
static unsigned offset;
static int delayed_read(Network *network, unsigned char *buffer, int length, int timeout)
{
    (void)network;
    int delay = offset ? 50 : 90;
    if (timeout < delay) { usleep(timeout * 1000); return 0; }
    usleep(delay * 1000);
    assert(offset + length <= incoming_length);
    memcpy(buffer, incoming + offset, length); offset += length; return length;
}
static int write_ack(Network *network,unsigned char *buffer,int length,int timeout)
{
 (void)network;assert(timeout>0);assert(length==4);assert(buffer[0]==0x40 && buffer[3]==7);acknowledgements++;return length;
}
int main(void)
{
    const unsigned char ack[] = {0x40,2,0,7};
    const unsigned char publication[] = {0x32,6,0,1,'x',0,7,'a'};
    incoming=ack;incoming_length=sizeof(ack);
    Network network = {0}; network.mqttread = delayed_read;network.mqttwrite=write_ack;
    Client client = {0}; assert(MQTTClientInit(&client, &network, 500) == 0);
    Timer yield; InitTimer(&yield); countdown_ms(&yield, 100);
    assert(readPacket(&client, &yield) == PUBACK);
    assert(offset == 4);
    assert(client.readbuf[3] == 7);
    offset=0;incoming=publication;incoming_length=sizeof(publication);
    client.isconnected=1;client.keepAliveInterval=30;client.heartbeat_retry_max=3;
    countdown(&client.ping_timer,30);countdown_ms(&yield,100);
    assert(cycle(&client,&yield)==PUBLISH);assert(acknowledgements==1);
    MQTTClientDeinit(&client);
    puts("M1/A1 complete fragmented MQTT packets and PUBACK beyond the idle yield deadline");
}
