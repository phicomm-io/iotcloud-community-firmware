#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "MQTTPacket.h"
#include "MQTTPublish.h"

static int decode(const unsigned char *packet, int length, int expected)
{
	unsigned char dup = 0xA5;
	unsigned char retained = 0xA5;
	int qos = 0x5A;
	unsigned short packetid = 0xA55A;
	MQTTString topic = MQTTString_initializer;
	unsigned char *payload = NULL;
	int payloadlen = -1;
	int result = MQTTDeserialize_publish(&dup, &qos, &retained, &packetid,
		&topic, &payload, &payloadlen, (unsigned char *)packet, length);
	if (result != expected)
		return 1;
	if (!expected) {
		assert(topic.lenstring.data == NULL);
		assert(payload == NULL);
		assert(payloadlen == -1);
		return 0;
	}
	assert(topic.cstring == NULL);
	assert(topic.lenstring.len > 0);
	assert(topic.lenstring.data == (char *)&packet[(packet[1] & 0x80) ? 5 : 4]);
	assert(qos >= 0 && qos <= 2);
	assert(payloadlen >= 0);
	assert(payload == &packet[length - payloadlen]);
	if (qos == 0)
		assert(packetid == 0xA55A);
	else
		assert(packetid != 0);
	return 0;
}

int main(void)
{
	static const unsigned char short_topic[] = {0x30, 2, 0, 5};
	static const unsigned char qos3[] = {0x36, 3, 0, 1, 't'};
	static const unsigned char empty_topic[] = {0x30, 2, 0, 0};
	static const unsigned char topic_overrun[] = {0x30, 3, 0, 2, 't'};
	static const unsigned char qos1_short_id[] = {0x32, 4, 0, 1, 't', 0};
	static const unsigned char qos1_zero_id[] = {0x32, 5, 0, 1, 't', 0, 0};
	static const unsigned char qos0[] = {0x30, 3, 0, 1, 't'};
	static const unsigned char qos1[] = {0x32, 5, 0, 1, 't', 0, 7};
	static const unsigned char qos2[] = {0x34, 5, 0, 1, 't', 0, 8};
	static unsigned char qos0_multibyte[131] = {0x30, 0x80, 0x01, 0, 1, 't'};
	static const unsigned char rl_truncated[] = {0x30, 0x80};
	static const unsigned char rl_noncanonical[] = {0x30, 0x83, 0x00, 0, 1, 't'};
	static const unsigned char rl_four_continues[] = {0x30, 0x80, 0x80, 0x80, 0x80};
	static const unsigned char packet_truncated[] = {0x30, 4, 0, 1, 't'};
	static const unsigned char missing_topic_len[] = {0x30, 1, 0};
	static const unsigned char truncated_payload[] = {0x30, 4, 0, 1, 't'};
	int i;
	int failed = 0;

	failed |= decode(short_topic, sizeof(short_topic), 0);
	failed |= decode(qos3, sizeof(qos3), 0);
	failed |= decode(empty_topic, sizeof(empty_topic), 0);
	failed |= decode(topic_overrun, sizeof(topic_overrun), 0);
	failed |= decode(qos1_short_id, sizeof(qos1_short_id), 0);
	failed |= decode(qos1_zero_id, sizeof(qos1_zero_id), 0);
	failed |= decode(rl_noncanonical, sizeof(rl_noncanonical), 0);
	failed |= decode(rl_four_continues, sizeof(rl_four_continues), 0);
	failed |= decode(missing_topic_len, sizeof(missing_topic_len), 0);
	failed |= decode(qos0, sizeof(qos0), 1);
	failed |= decode(qos1, sizeof(qos1), 1);
	failed |= decode(qos2, sizeof(qos2), 1);
	failed |= decode(qos0_multibyte, sizeof(qos0_multibyte), 1);
	for (i = 0; i < (int)sizeof(rl_truncated); ++i)
		failed |= decode(rl_truncated, i, 0);
	for (i = 0; i < (int)sizeof(packet_truncated); ++i)
		failed |= decode(packet_truncated, i, 0);
	for (i = 0; i < (int)sizeof(truncated_payload); ++i)
		failed |= decode(truncated_payload, i, 0);
	if (failed) {
		fprintf(stderr, "MQTT PUBLISH bounds regression failed\n");
		return 1;
	}
	puts("MQTT PUBLISH bounds regression passed");
	return 0;
}
