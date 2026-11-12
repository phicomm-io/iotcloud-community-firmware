#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#define SCREEN_MSG_PAGE_REQUEST 0x37u
#define SCREEN_MSG_PAGE_STATUS 0x38u
#define SCREEN_PAGE_REQUEST_BYTES 12u
#define SCREEN_PAGE_STATUS_BYTES 16u
/* Requests: op, version, action (query/goto/next/previous), zero, id:u32,
 * page:u8 (one based for goto only), three zeros. Status: op,version,result,
 * asleep, id:u32 (zero for telemetry), current:u8, enabled_mask:u8,
 * count:u8, zero, applied_layout_revision:u32. Remote changes wake the display;
 * neither the layout nor the saved sleep policy is changed. */
static inline bool screen_page_request_valid(const uint8_t *p,size_t n){
  if(n!=SCREEN_PAGE_REQUEST_BYTES||p[0]!=SCREEN_MSG_PAGE_REQUEST||p[1]!=1||
     p[2]>3||p[3]||!(p[4]|p[5]|p[6]|p[7])||p[9]||p[10]||p[11])return false;
  return p[2]==1 ? p[8]>=1&&p[8]<=8 : p[8]==0;
}
