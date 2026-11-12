/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define MEDIA_CHUNK 512u
#define MEDIA_PACKET_MAX 640u
#define MEDIA_JPEG_MAX (128u * 1024u)
#define MEDIA_WIDTH_MAX 480u
#define MEDIA_HEIGHT_MAX 854u
#define MEDIA_QUEUE 16u
#define MEDIA_IMAGE_SLOTS 4u
#define MEDIA_STACK 4u
#define MEDIA_HISTORY 16u
#define MEDIA_STATUS_IMAGES 32u
#define MEDIA_STATUS_QUEUE (MEDIA_STATUS_IMAGES + MEDIA_IMAGE_SLOTS * 17u)
#define MEDIA_STATUS_HISTORY (MEDIA_STATUS_QUEUE + MEDIA_QUEUE * 9u)
#define MEDIA_STATUS_BYTES (MEDIA_STATUS_HISTORY + MEDIA_HISTORY * 5u)
#define MEDIA_STREAM_STATUS_BYTES 40u
#define MEDIA_DECODER_DETAIL_BYTES 64u
#define MEDIA_TAP_WINDOW_MS 350u
/* Zero-delimited, escaped binary records are consumed before either text
 * parser. Header: GM, version, channel, boot session LE32, sequence LE32,
 * payload length LE16; CRC32 covers header and payload. No Flash writes. */
enum {
  MEDIA_LINK = 0,
  MEDIA_COMMAND = 1,
  MEDIA_REPLY = 2,
  MEDIA_SOCKET = 3,
  MEDIA_SOCKET_ACK = 4,
  MEDIA_SOCKET_EVENT = 5
};
enum {
  MEDIA_PING = 1,
  MEDIA_PONG,
  MEDIA_BAUD,
  MEDIA_BAUD_READY,
  MEDIA_BAUD_PROBE,
  MEDIA_BAUD_CONFIRM
};
enum {
  MEDIA_BEGIN = 1,
  MEDIA_WRITE,
  MEDIA_COMMIT,
  MEDIA_CANCEL,
  MEDIA_PUSH,
  MEDIA_DISMISS,
  MEDIA_QUERY,
  MEDIA_STREAM_CONFIG,
  MEDIA_STREAM_START,
  MEDIA_STREAM_STOP,
  MEDIA_SELECT
};
enum {
  MEDIA_OK = 0,
  MEDIA_RANGE,
  MEDIA_BUSY,
  MEDIA_NOT_FOUND,
  MEDIA_CRC,
  MEDIA_FORMAT,
  MEDIA_DECODE,
  MEDIA_OFFLINE
};
enum {
  PUSH_EMPTY = 0,
  PUSH_QUEUED,
  PUSH_DISPLAYED,
  PUSH_CONFIRMED,
  PUSH_TIMEDOUT,
  PUSH_DISMISSED,
  PUSH_FAILED
};
enum {
  MEDIA_NET_OPEN = 1,
  MEDIA_NET_WRITE,
  MEDIA_NET_CLOSE,
  MEDIA_NET_CONNECTED,
  MEDIA_NET_CLOSED,
  MEDIA_NET_ERROR
};
typedef void (*media_packet_fn)(void *, unsigned, uint32_t, uint32_t,
                                const uint8_t *, size_t);
typedef struct {
  uint8_t bytes[MEDIA_PACKET_MAX + 18];
  uint16_t used;
  bool active, escape, discard;
  uint32_t at, crc_errors, framing_errors;
  void *context;
  media_packet_fn packet;
} media_parser;
uint32_t media_crc32(const void *, size_t);
uint32_t media_u32(const uint8_t *);
void media_put32(uint8_t *, uint32_t);
/* Returns true for every binary byte, including corrupt/oversize frames. */
bool media_feed(media_parser *, unsigned, uint32_t);
size_t media_encode(uint8_t *, size_t, unsigned, uint32_t, uint32_t,
                    const void *, size_t);
bool media_rate_valid(uint32_t);
