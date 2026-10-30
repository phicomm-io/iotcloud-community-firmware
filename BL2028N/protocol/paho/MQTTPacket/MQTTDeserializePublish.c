/*******************************************************************************
 * Copyright (c) 2014 IBM Corp.
 *
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the Eclipse Public License v1.0
 * and Eclipse Distribution License v1.0 which accompany this distribution.
 *
 * The Eclipse Public License is available at
 *    http://www.eclipse.org/legal/epl-v10.html
 * and the Eclipse Distribution License is available at
 *   http://www.eclipse.org/org/documents/edl-v10.php.
 *
 * Contributors:
 *    Ian Craggs - initial API and implementation and/or initial documentation
 *******************************************************************************/

#include "StackTrace.h"
#include "MQTTPacket.h"
#include <string.h>

#define min(a, b) ((a < b) ? 1 : 0)

/**
  * Deserializes the supplied (wire) buffer into publish data
  * @param dup returned integer - the MQTT dup flag
  * @param qos returned integer - the MQTT QoS value
  * @param retained returned integer - the MQTT retained flag
  * @param packetid returned integer - the MQTT packet identifier
  * @param topicName returned MQTTString - the MQTT topic in the publish
  * @param payload returned byte buffer - the MQTT publish payload
  * @param payloadlen returned integer - the length of the MQTT payload
  * @param buf the raw buffer data, of the correct length determined by the remaining length field
  * @param buflen the length in bytes of the data in the supplied buffer
  * @return error code.  1 is success
  */
int MQTTDeserialize_publish(unsigned char* dup, int* qos, unsigned char* retained, unsigned short* packetid, MQTTString* topicName,
		unsigned char** payload, int* payloadlen, unsigned char* buf, int buflen)
{
	MQTTHeader header = {0};
	int rc = 0;
	int offset = 0;
	int remaining = 0;
	int multiplier = 1;
	int length_bytes;
	int topic_length;
	int payload_offset;
	int packet_qos;
	int packet_dup;
	int packet_retained;
	unsigned short packet_identifier = 0;
	unsigned char* packet_payload;
	int packet_payloadlen;
	MQTTString packet_topic = MQTTString_initializer;

	FUNC_ENTRY;
	if (!buf || buflen < 2 || !dup || !qos || !retained || !packetid ||
		!topicName || !payload || !payloadlen)
		goto exit;
	header.byte = buf[offset++];
	if (header.bits.type != PUBLISH)
		goto exit;
	packet_qos = header.bits.qos;
	if (packet_qos == 3)
		goto exit;
	packet_dup = header.bits.dup;
	packet_retained = header.bits.retain;

	/* Decode only bytes present in this buffer; the generic SDK decoder is unbounded. */
	for (length_bytes = 0; length_bytes < 4; ++length_bytes)
	{
		unsigned char encoded;
		if (offset >= buflen)
			goto exit;
		encoded = buf[offset++];
		remaining += (encoded & 0x7f) * multiplier;
		if ((encoded & 0x80) == 0)
			break;
		if (length_bytes == 3)
			goto exit;
		multiplier *= 128;
	}
	if (length_bytes == 4 || remaining > buflen - offset)
		goto exit;
	if (length_bytes > 0 && remaining < multiplier)
		goto exit;
	if (remaining < 2)
		goto exit;

	/* Topic length and data must fit within the declared packet body. */
	topic_length = (buf[offset] << 8) | buf[offset + 1];
	if (topic_length == 0 || topic_length > remaining - 2)
		goto exit;
	packet_topic.lenstring.len = topic_length;
	packet_topic.lenstring.data = (char *)&buf[offset + 2];
	packet_topic.cstring = NULL;
	payload_offset = offset + 2 + topic_length;

	if (packet_qos > 0)
	{
		if (offset + remaining - payload_offset < 2)
			goto exit;
		packet_identifier = (buf[payload_offset] << 8) | buf[payload_offset + 1];
		if (packet_identifier == 0)
			goto exit;
		payload_offset += 2;
	}

	packet_payloadlen = offset + remaining - payload_offset;
	packet_payload = &buf[payload_offset];
	*dup = packet_dup;
	*qos = packet_qos;
	*retained = packet_retained;
	*topicName = packet_topic;
	if (packet_qos > 0)
		*packetid = packet_identifier;
	*payloadlen = packet_payloadlen;
	*payload = packet_payload;
	rc = 1;
exit:
	FUNC_EXIT_RC(rc);
	return rc;
}



/**
  * Deserializes the supplied (wire) buffer into an ack
  * @param packettype returned integer - the MQTT packet type
  * @param dup returned integer - the MQTT dup flag
  * @param packetid returned integer - the MQTT packet identifier
  * @param buf the raw buffer data, of the correct length determined by the remaining length field
  * @param buflen the length in bytes of the data in the supplied buffer
  * @return error code.  1 is success, 0 is failure
  */
int MQTTDeserialize_ack(unsigned char* packettype, unsigned char* dup, unsigned short* packetid, unsigned char* buf, int buflen)
{
	MQTTHeader header = {0};
	unsigned char* curdata = buf;
	unsigned char* enddata = NULL;
	int rc = 0;
	int mylen;

	FUNC_ENTRY;
	header.byte = readChar(&curdata);
	*dup = header.bits.dup;
	*packettype = header.bits.type;

	curdata += (rc = MQTTPacket_decodeBuf(curdata, &mylen)); /* read remaining length */
	enddata = curdata + mylen;

	if (enddata - curdata < 2)
		goto exit;
	*packetid = readInt(&curdata);

	rc = 1;
exit:
	FUNC_EXIT_RC(rc);
	return rc;
}

