#include "MQTTClient.h"
#include "MQTTPublish.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef NEGATIVE_CONTROL
#define NEGATIVE_CONTROL 0
#endif

typedef union allocation allocation;
union allocation {
    long double align;
    struct { size_t capacity; } meta;
};

static size_t live_bytes, peak_bytes;
static unsigned realloc_calls;
static int fail_next_malloc;
static int fail_next_permit;
static unsigned char incoming[8192];
static size_t incoming_size, incoming_at;
static unsigned char outgoing[8192];
static size_t outgoing_size;
static int inject_puback;
static unsigned short injected_puback_id;
static unsigned received_callbacks;
static unsigned char *callback_payload_view;
static size_t callback_payload_size;

void *__real_malloc(size_t);
void __real_free(void *);
void *__real_realloc(void *, size_t);

int mqtt_buffer_allocation_permit(Client *client, size_t bytes)
{
    (void)client;
    assert(bytes > 0);
    if (fail_next_permit) {
        fail_next_permit = 0;
        return 0;
    }
    return 1;
}

void *__wrap_malloc(size_t size)
{
    if (fail_next_malloc) {
        fail_next_malloc = 0;
        return NULL;
    }
    allocation *block = __real_malloc(sizeof(*block) + size);
    if (!block) return NULL;
    block->meta.capacity = size;
    memset(block + 1, 0xa5, size);
    live_bytes += size;
    if (live_bytes > peak_bytes) peak_bytes = live_bytes;
    return block + 1;
}

void __wrap_free(void *ptr)
{
    if (!ptr) return;
    allocation *block = (allocation *)ptr - 1;
    assert(live_bytes >= block->meta.capacity);
    live_bytes -= block->meta.capacity;
    __real_free(block);
}

static size_t actual_capacity(void *ptr)
{
    assert(ptr);
    return ((allocation *)ptr - 1)->meta.capacity;
}

void *__wrap_realloc(void *ptr, size_t size)
{
    ++realloc_calls;
    if (!ptr) return __wrap_malloc(size);
    if (!size) { __wrap_free(ptr); return NULL; }
    size_t old_size = actual_capacity(ptr);
    if (size < old_size) return ptr; /* Model the pinned kernel realloc shrink no-op. */
    if (fail_next_malloc) {
        fail_next_malloc = 0;
        return NULL;
    }
    void *next = __wrap_malloc(size);
    if (!next) return NULL;
    memcpy(next, ptr, old_size);
    __wrap_free(ptr);
    return next;
}

void *__wrap_calloc(size_t count, size_t size)
{
    if (size && count > SIZE_MAX / size) return NULL;
    void *ptr = __wrap_malloc(count * size);
    if (ptr) memset(ptr, 0, count * size);
    return ptr;
}

char *__wrap_strdup(const char *value)
{
    size_t size = strlen(value) + 1;
    char *copy = __wrap_malloc(size);
    if (copy) memcpy(copy, value, size);
    return copy;
}

char expired(Timer *timer) { return (int32_t)(0u - (uint32_t)timer->end_time) >= 0; }
void countdown_ms(Timer *timer, unsigned int ms) { timer->end_time = ms; }
void countdown(Timer *timer, unsigned int seconds) { countdown_ms(timer, seconds * 1000u); }
int left_ms(Timer *timer) { return (int)timer->end_time; }
void InitTimer(Timer *timer) { memset(timer, 0, sizeof(*timer)); }

static int read_bytes(Network *network, unsigned char *buffer, int count, int timeout)
{
    (void)network; (void)timeout;
    size_t available = incoming_size - incoming_at;
    size_t copied = count < 0 ? 0 : (size_t)count;
    if (copied > available) copied = available;
    if (!copied) return 0;
    memcpy(buffer, incoming + incoming_at, copied);
    incoming_at += copied;
    return (int)copied;
}

static int write_bytes(Network *network, unsigned char *buffer, int count, int timeout)
{
    (void)network; (void)timeout;
    assert(count >= 0 && outgoing_size + (size_t)count <= sizeof(outgoing));
    memcpy(outgoing + outgoing_size, buffer, (size_t)count);
    outgoing_size += (size_t)count;
    if (inject_puback && count >= 6 && buffer[0] == 0x32) {
        size_t offset = 1;
        while (buffer[offset++] & 0x80) assert(offset < (size_t)count);
        unsigned topic_size = ((unsigned)buffer[offset] << 8) | buffer[offset + 1];
        size_t id_offset = offset + 2 + topic_size;
        assert(id_offset + 2 <= (size_t)count);
        incoming[0] = 0x40;
        incoming[1] = 2;
        incoming[2] = buffer[id_offset];
        incoming[3] = buffer[id_offset + 1];
        injected_puback_id = (unsigned short)(((unsigned)incoming[2] << 8) | incoming[3]);
        incoming_size = 4;
        incoming_at = 0;
        inject_puback = 0;
    }
    return count;
}

static void on_message(MessageData *data)
{
    assert(data->topicName->lenstring.len == strlen("rx/telemetry"));
    assert(memcmp(data->topicName->lenstring.data, "rx/telemetry",
                  data->topicName->lenstring.len) == 0);
    callback_payload_view = data->message->payload;
    callback_payload_size = data->message->payloadlen;
    assert(callback_payload_size == 1800 || callback_payload_size == 16);
    if (callback_payload_size == 1800)
        for (size_t i = 0; i < callback_payload_size; ++i)
            assert(callback_payload_view[i] == (unsigned char)('a' + (i % 23)));
    ++received_callbacks;
#if !NEGATIVE_CONTROL
    if (callback_payload_size == 1800) fail_next_malloc = 1;
#endif
}

static void queue_publish(const char *topic, const unsigned char *payload, size_t size)
{
    MQTTString name = MQTTString_initializer;
    name.cstring = (char *)topic;
    int encoded = MQTTSerialize_publish(incoming, sizeof(incoming), 0, 1, 0, 17,
                                         name, (unsigned char *)payload, (int)size);
    assert(encoded > 0);
    incoming_size = (size_t)encoded;
    incoming_at = 0;
}

static int storage_is_truthful(Client *client, int send)
{
    void *buffer = send ? (void *)client->buf : (void *)client->readbuf;
    size_t reported = send ? client->buf_size : client->readbuf_size;
    return actual_capacity(buffer) == reported;
}

static void run_scenario(void)
{
    Network network = {0};
    network.mqttread = read_bytes;
    network.mqttwrite = write_bytes;
    Client client = {0};
    assert(MQTTClientInit(&client, &network, 1000) == MQTT_SUCCESS);
    assert(storage_is_truthful(&client, 1) && storage_is_truthful(&client, 0));
    client.isconnected = 1;
    client.keepAliveInterval = 30;
    client.heartbeat_retry_max = 1;
    client.messageHandlers[0].topicFilter = "rx/#";
    client.messageHandlers[0].fp = on_message;
    outgoing_size = 0;

    unsigned char body[2400];
    for (size_t i = 0; i < sizeof(body); ++i) body[i] = (unsigned char)('A' + (i % 19));
    MQTTMessage message = {0};
    message.qos = QOS0;
    message.payload = body;
    message.payloadlen = sizeof(body);
#if !NEGATIVE_CONTROL
    unsigned char *original_tx = client.buf;
    size_t original_tx_capacity = client.buf_size;
    fail_next_malloc = 1;
    assert(MQTTPublish(&client, "tx/bulk", &message) == MQTT_BUFFER_OVERFLOW);
    assert(client.buf == original_tx && client.buf_size == original_tx_capacity);
    assert(storage_is_truthful(&client, 1) && outgoing_size == 0);
#endif
    assert(MQTTPublish(&client, "tx/bulk", &message) == MQTT_SUCCESS);
    assert(outgoing_size > sizeof(body));
    assert(memcmp(outgoing + outgoing_size - sizeof(body), body, sizeof(body)) == 0);
#if NEGATIVE_CONTROL
    assert(actual_capacity(client.buf) > client.buf_size);
#else
    assert(storage_is_truthful(&client, 1));
#endif
    assert(client.buf_size == DEFAULT_SENDBUF_SIZE);
    outgoing_size = 0;

    unsigned char rx_body[1800];
    for (size_t i = 0; i < sizeof(rx_body); ++i) rx_body[i] = (unsigned char)('a' + (i % 23));
#if !NEGATIVE_CONTROL
    queue_publish("rx/telemetry", rx_body, sizeof(rx_body));
    fail_next_permit = 1;
    assert(MQTTYield(&client, 10) == MQTT_FAILURE);
    assert(received_callbacks == 0 && storage_is_truthful(&client, 0));
    assert(client.readbuf_size == DEFAULT_READBUF_SIZE);
    queue_publish("rx/telemetry", rx_body, sizeof(rx_body));
    fail_next_malloc = 1;
    assert(MQTTYield(&client, 10) == MQTT_FAILURE);
    assert(received_callbacks == 0 && storage_is_truthful(&client, 0));
    assert(client.readbuf_size == DEFAULT_READBUF_SIZE);
#endif
    queue_publish("rx/telemetry", rx_body, sizeof(rx_body));
    assert(MQTTYield(&client, 10) == MQTT_SUCCESS);
    assert(received_callbacks == 1);
    assert(callback_payload_view >= client.readbuf &&
           callback_payload_view + callback_payload_size <= client.readbuf + actual_capacity(client.readbuf));
#if NEGATIVE_CONTROL
    assert(actual_capacity(client.readbuf) > client.readbuf_size);
    assert(realloc_calls > 0);
#else
    assert(storage_is_truthful(&client, 0));
    assert(realloc_calls == 0);
    assert(client.readbuf_size > DEFAULT_READBUF_SIZE);
#endif

    /* A following PUBACK is decoded only after cycle() attempts to shrink RX. */
    MQTTMessage acknowledged = {0};
    acknowledged.qos = QOS1;
    acknowledged.payload = "ack";
    acknowledged.payloadlen = 3;
    inject_puback = 1;
    assert(MQTTPublish(&client, "tx/ack", &acknowledged) == MQTT_SUCCESS);
    assert(acknowledged.id != 0 && acknowledged.id == injected_puback_id && !inject_puback);
    assert(client.readbuf_size == DEFAULT_READBUF_SIZE);
#if NEGATIVE_CONTROL
    assert(actual_capacity(client.readbuf) > client.readbuf_size);
#else
    assert(storage_is_truthful(&client, 0));
#endif

    /* Repeated mixed-size TX/RX cycles expose stale capacity and retained peaks. */
    size_t bounded_peak = peak_bytes;
    for (unsigned cycle = 0; cycle < 1000; ++cycle) {
        message.payloadlen = cycle % 2 ? 20 : sizeof(body);
        assert(MQTTPublish(&client, "tx/cycle", &message) == MQTT_SUCCESS);
#if NEGATIVE_CONTROL
        assert(actual_capacity(client.buf) > client.buf_size);
#else
        assert(storage_is_truthful(&client, 1));
#endif
        outgoing_size = 0;
        unsigned char small[16] = "short-message";
        queue_publish("rx/telemetry", small, sizeof(small));
        assert(MQTTYield(&client, 10) == MQTT_SUCCESS);
#if NEGATIVE_CONTROL
        assert(actual_capacity(client.readbuf) > client.readbuf_size);
#else
        assert(storage_is_truthful(&client, 0));
#endif
        assert(peak_bytes <= bounded_peak + 4096);
    }
    assert(received_callbacks == 1001);
    assert(MQTTClientDeinit(&client) == MQTT_SUCCESS);
    assert(live_bytes == 0);
}

int main(void)
{
    run_scenario();
#if NEGATIVE_CONTROL
    puts("negative control reproduced retained RX allocation beyond reported capacity");
#else
    puts("fixed MQTT buffer storage kept reported and actual capacities aligned");
#endif
    return 0;
}
