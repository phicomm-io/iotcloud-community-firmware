#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
/* Negotiated, read-only instrumentation; no LCD timing/power/Flash writes.
 * Requests: opcode, version, action, mode, id:u32, offset:u16, limit:u8,
 * page:u8 (one based), four zero bytes. Actions 1=capture, 2=read, 3=stop.
 * Modes 0=redraw current, 1=wait for next page, 2=select enabled page.
 * Reply: 128-byte summary followed by at most 8 24-byte trace records.
 * All integers little endian; counters saturate at the trace bound only in
 * storage: summary counts/totals continue and report dropped records. */
#define SCREEN_MSG_DIAG_REQUEST 0x35u
#define SCREEN_MSG_DIAG_REPLY 0x36u
#define SCREEN_DIAG_REQUEST_BYTES 16u
#define SCREEN_DIAG_SUMMARY_BYTES 128u
#define SCREEN_DIAG_RECORD_BYTES 24u
#define SCREEN_DIAG_BATCH 8u
#define SCREEN_DIAG_TRACE 96u
#define SCREEN_DIAG_REPLY_MAX (SCREEN_DIAG_SUMMARY_BYTES+SCREEN_DIAG_RECORD_BYTES*SCREEN_DIAG_BATCH)
static inline bool screen_diag_request_valid(const uint8_t *p,size_t n) {
  if(n!=SCREEN_DIAG_REQUEST_BYTES||p[0]!=SCREEN_MSG_DIAG_REQUEST||p[1]!=1||
     p[2]<1||p[2]>3||p[3]>2||!(p[4]|p[5]|p[6]|p[7])||p[10]>SCREEN_DIAG_BATCH||
     p[12]||p[13]||p[14]||p[15])return false;
  if(p[2]==1)return !(p[8]|p[9]|p[10])&&((p[3]==2)?p[11]>=1&&p[11]<=8:p[11]==0);
  if(p[2]==2)return !p[3]&&!p[11]&&(unsigned)(p[8]|p[9]<<8)<=SCREEN_DIAG_TRACE;
  return !(p[3]|p[8]|p[9]|p[10]|p[11]);
}
