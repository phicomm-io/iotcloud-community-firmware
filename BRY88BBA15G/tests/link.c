/* Cross-repository test: these are the actual endpoints, not mock decoders. */
#include "bridge.h"
#include "runtime.h"
#include "widget_fixtures.h"
#include "wallpaper.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static bry_bridge bridge;
static screen_runtime screen;
static bool fail_send;
static unsigned backlight_reports;
static screen_backlight backlight;
static uint8_t appearance_ack[7],wallpaper_ack[20],page_status[SCREEN_PAGE_STATUS_BYTES];
static bool module_high;static unsigned module_reports;static uint8_t module_status[SCREEN_MODULE_STATUS_BYTES];
static void module_write(void *context,bool high){(void)context;module_high=high;}
static bool module_read(void *context){(void)context;return module_high;}
static void extension(void *context, const void *data, size_t n) {
  (void)context;
  const uint8_t *p = data;
  if(n==SCREEN_PAGE_STATUS_BYTES&&p[0]==SCREEN_MSG_PAGE_STATUS)memcpy(page_status,p,n);
  if (n && p[0] == SCREEN_MSG_BACKLIGHT) {
    assert(screen_backlight_decode(&backlight, p, n));
    backlight_reports++;
  }
  if(n==SCREEN_MODULE_STATUS_BYTES&&p[0]==SCREEN_MSG_MODULE_STATUS){memcpy(module_status,p,n);module_reports++;}
  if(n==7&&p[0]==SC_MSG_ACK&&p[1]==SCREEN_MSG_APPEARANCE)memcpy(appearance_ack,p,n);
  if(n==20&&p[0]==SCREEN_MSG_WALLPAPER_STATUS)memcpy(wallpaper_ack,p,n);
}
static bool to_screen(void *context, const char *p, size_t n) {
  (void)context;
  if (fail_send)
    return false;
  for (size_t i = 0; i < n; i++)
    screen_runtime_feed(&screen, p + i, 1, 1000);
  return true;
}
static bool to_esp(void *context, const char *p, size_t n) {
  (void)context;
  for (size_t i = 0; i < n; i++)
    bry_bridge_feed(&bridge, p + i, 1);
  return true;
}
int main(void) {
  bridge.send = to_screen;
  bridge.extension = extension;
  bry_bridge_init(&bridge);
  screen.send = to_esp;
  screen_runtime_init(&screen);
  assert(!bry_bridge_publish(&bridge, demo_widget_4, sizeof(demo_widget_4)));
  fail_send = true;
  assert(screen_runtime_offer(&screen, 0x11223344));
  assert(!screen.caps.ready && !bridge.extended);
  assert(!bry_bridge_publish(&bridge, demo_widget_0, sizeof(demo_widget_0)));
  screen_runtime_init(&screen);
  fail_send = false;
  assert(screen_runtime_offer(&screen, 0x12345678));
  assert(screen.caps.ready && bridge.extended);
  assert(screen.caps.capabilities == (SCREEN_CAP_KNOWN & ~SCREEN_CAP_FLASH_INFO));
  assert(bridge.capabilities == screen.caps.capabilities);

  assert(screen_runtime_backlight(&screen, 60, 10, 10, false));
  assert(backlight_reports == 1 && backlight.applied == 10);
  assert(screen_runtime_backlight(&screen, 5, 10, 0, true));
  assert(backlight_reports == 2 && backlight.asleep && !backlight.applied);
  assert(bry_bridge_publish(&bridge, demo_widget_4, sizeof(demo_widget_4)));
  assert(screen.model.external.items[1].type == 4 && screen.last_sequence == 1);
  fail_send = true;
  assert(!bry_bridge_publish(&bridge, demo_widget_0, sizeof(demo_widget_0)));
  assert(bridge.tx_sequence == 1);
  fail_send = false;
  assert(bry_bridge_publish(&bridge, demo_widget_0, sizeof(demo_widget_0)));
  assert(screen.last_sequence == 2 && screen.model.external.items[1].type == 0);
  assert(!bry_bridge_publish(&bridge, "bad", 3));
  assert(bridge.tx_sequence == 2);
  /* Replayed capability offers cannot rewind transmit or receive sequences. */
  char offer[80];
  screen_caps caps;
  size_t n = screen_caps_offer(&caps, offer, sizeof(offer), 0x12345678, 1);
  bry_bridge_feed(&bridge, offer, n);
  assert(bridge.tx_sequence == 2);

  assert(screen.last_sequence ==
         2); /* Replayed ACK cannot rewind MCU receive state. */
  screen_scene scene;
  scene_defaults(&scene);
  scene.revision = 2;
  scene.page[3].enabled = true;
  scene.page[4].enabled = true;
  scene.page[5].enabled = true;
  uint8_t bytes[SCENE_BYTES], begin[13] = {SC_MSG_BEGIN},
                              chunk[517] = {SC_MSG_CHUNK},
                              commit[5] = {SC_MSG_COMMIT};
  assert(scene_encode(&scene, bytes));
  scene_put32(begin + 1, 2);
  scene_put32(begin + 5, SCENE_BYTES);
  scene_put32(begin + 9, scene_crc32(bytes, sizeof(bytes)));
  assert(bry_bridge_send(&bridge, begin, sizeof(begin), SCREEN_CAP_SCENE));
  assert(screen.model.scene.revision == 1);
  for (unsigned at = 0; at < sizeof(bytes); at += 512) {
    unsigned n = sizeof(bytes) - at;
    if (n > 512)
      n = 512;
    scene_put32(chunk + 1, at);
    memcpy(chunk + 5, bytes + at, n);
    assert(bry_bridge_send(&bridge, chunk, n + 5, SCREEN_CAP_SCENE));
    assert(screen.model.scene.revision == 1);
  }
  scene_put32(commit + 1, 2);
  assert(bry_bridge_send(&bridge, commit, sizeof(commit), SCREEN_CAP_SCENE));
  assert(screen.model.scene.revision == 2);
  uint8_t data[15] = {SC_MSG_DATA};
  data[1] = 16;
  data[3] = 1;
  scene_put32(data + 4, 10);
  data[8] = 2;
  memcpy(data + 9, "OK", 2);
  float value = 23.5;
  uint32_t f;
  memcpy(&f, &value, 4);
  scene_put32(data + 11, f);
  assert(bry_bridge_send(&bridge, data, sizeof(data), SCREEN_CAP_SCENE));
  unsigned external = 0;
  while (external < screen.model.scene.count &&
         screen.model.scene.item[external].id != 16)
    external++;
  assert(external < screen.model.scene.count);
  assert(screen.model.data[external].id == 16 &&
         screen.model.data[external].values[0] == 23.5f);
  uint8_t clock[7] = {SC_MSG_CLOCK};
  scene_put32(clock + 1, 1793880000);
  clock[5] = 0xe0;
  clock[6] = 1;
  assert(bry_bridge_send(&bridge, clock, sizeof(clock), SCREEN_CAP_SERVICES));
  assert(screen.clock_epoch == 1793880000 && screen.timezone == 480);
  uint8_t notice[204] = {SC_MSG_REMINDER};
  scene_put32(notice + 1, 123);
  scene_put32(notice + 5, 1793880000);
  notice[9] = REM_RESPECT_DND;
  notice[10] = 1;
  strcpy((char *)notice + 11, "Ventilate");
  strcpy((char *)notice + 76, "Open the window");
  screen.model.asleep = true;
  screen.model.dnd = true;
  assert(bry_bridge_send(&bridge, notice, sizeof(notice), SCREEN_CAP_SERVICES));
  assert(screen.model.asleep && screen.model.reminders[0].unread);
  notice[9] = REM_ALWAYS_WAKE;
  assert(bry_bridge_send(&bridge, notice, sizeof(notice), SCREEN_CAP_SERVICES));
  assert(!screen.model.asleep && screen.model.page == 4);
  bry_bridge_feed(&bridge, "version?\r\n", 10);
  assert(!bridge.extended);
  assert(!bry_bridge_publish(&bridge, demo_widget_0, sizeof(demo_widget_0)));
  screen_runtime_init(&screen);
  assert(screen_runtime_offer(&screen, 0x23456789));
  assert(bry_bridge_publish(&bridge, demo_widget_2, sizeof(demo_widget_2)));
  assert(screen.last_sequence == 1);
  screen_runtime_tick(&screen, 301000);
  assert(!screen_widget_valid(&screen.model.external.items[1], 301000));
  /* Obsolete community offers never activate binary extensions. */
  bry_bridge_reset(&bridge);
  n=screen_caps_offer(&screen.caps,offer,sizeof(offer),0x34567890,
                      SCREEN_CAP_SCENE|SCREEN_CAP_BACKLIGHT|SCREEN_CAP_SERVICES);
  assert(to_esp(NULL,offer,n));
  assert(!bridge.extended);
  screen_runtime_init(&screen);
  screen_module_init(&screen.module,NULL,module_write,module_read);
  assert(screen_runtime_offer(&screen,0x456789ab));
  assert((bridge.capabilities&SCREEN_CAP_MODULE_CONTROL)&&module_high);
  uint8_t module[SCREEN_MODULE_REQUEST_BYTES]={SCREEN_MSG_MODULE_REQUEST,1,SCREEN_MODULE_LOW};
  scene_put32(module+4,77);scene_put32(module+8,1000);
  assert(bry_bridge_send(&bridge,module,sizeof(module),SCREEN_CAP_MODULE_CONTROL));
  assert(!module_high&&module_reports==1&&module_status[2]==0&&scene_u32(module_status+4)==77);
  assert((module_status[3]&7)==5&&scene_u32(module_status+8)==1000);
  screen_runtime_tick(&screen,2000);assert(module_high&&!screen.module.duration);
  module[2]=SCREEN_MODULE_HIGH;scene_put32(module+4,78);scene_put32(module+8,0);
  assert(bry_bridge_send(&bridge,module,sizeof(module),SCREEN_CAP_MODULE_CONTROL));
  assert(module_high&&module_status[2]==0&&(module_status[3]&7)==3&&scene_u32(module_status+4)==78);
  uint8_t appearance[APPEARANCE_BYTES+1]={SCREEN_MSG_APPEARANCE};
  screen_appearance a;appearance_default(&a);a.revision=2;a.theme=5;a.wallpaper=1;
  assert(appearance_encode(&a,appearance+1,APPEARANCE_BYTES));
  assert(bry_bridge_send(&bridge,appearance,sizeof(appearance),SCREEN_CAP_APPEARANCE));
  assert(screen.model.appearance.theme==5&&wallpaper.wanted==1);
  assert(!appearance_ack[2]&&scene_u32(appearance_ack+3)==2);
  appearance[11]=6;
  assert(bry_bridge_send(&bridge,appearance,sizeof(appearance),SCREEN_CAP_APPEARANCE));
  assert(appearance_ack[2]==1&&screen.model.appearance.theme==5);
  uint8_t query[12]={SCREEN_MSG_WALLPAPER,1,WP_QUERY};scene_put32(query+4,123);
  assert(bry_bridge_send(&bridge,query,sizeof(query),SCREEN_CAP_WALLPAPER));
  assert(!wallpaper_ack[2]&&scene_u32(wallpaper_ack+4)==123);
  uint8_t diagnostic[SCREEN_DIAG_REQUEST_BYTES]={SCREEN_MSG_DIAG_REQUEST,1,1,0,1};
  assert(bry_bridge_send(&bridge,diagnostic,sizeof(diagnostic),SCREEN_CAP_DISPLAY_DIAG));
  assert(screen.diag_pending&&!memcmp(screen.diag_request,diagnostic,sizeof(diagnostic)));
  // An unconsumed request cannot be overwritten by a second sender.
  diagnostic[4]=2;
  assert(bry_bridge_send(&bridge,diagnostic,sizeof(diagnostic),SCREEN_CAP_DISPLAY_DIAG));
  assert(screen.diag_request[4]==1);screen.diag_pending=false;
  diagnostic[12]=1;
  assert(bry_bridge_send(&bridge,diagnostic,sizeof(diagnostic),SCREEN_CAP_DISPLAY_DIAG));
  assert(!screen.diag_pending);

  screen.model.scene.pages=6;
  for(unsigned i=0;i<6;i++)screen.model.scene.page[i].enabled=i==0||i==2||i==5;
  screen.model.page=0;screen.model.asleep=true;
  uint8_t page_request[SCREEN_PAGE_REQUEST_BYTES]={SCREEN_MSG_PAGE_REQUEST,1,2,0,1};
  assert(bry_bridge_send(&bridge,page_request,sizeof(page_request),SCREEN_CAP_PAGE_CONTROL));
  assert(screen.model.page==2&&!screen.model.asleep&&screen.reminder_wake&&!page_status[2]&&page_status[8]==3);
  page_request[2]=3;page_request[4]++;
  assert(bry_bridge_send(&bridge,page_request,sizeof(page_request),SCREEN_CAP_PAGE_CONTROL));
  assert(screen.model.page==0&&page_status[8]==1);
  page_request[2]=1;page_request[8]=2;page_request[4]++;
  assert(bry_bridge_send(&bridge,page_request,sizeof(page_request),SCREEN_CAP_PAGE_CONTROL));
  assert(screen.model.page==0&&page_status[2]==1);
  page_request[8]=6;page_request[4]++;
  assert(bry_bridge_send(&bridge,page_request,sizeof(page_request),SCREEN_CAP_PAGE_CONTROL));
  assert(screen.model.page==5&&!page_status[2]);
  page_request[2]=2;page_request[8]=0;page_request[4]++;
  assert(bry_bridge_send(&bridge,page_request,sizeof(page_request),SCREEN_CAP_PAGE_CONTROL));
  assert(screen.model.page==0);
  screen.model.ota=true;page_request[4]++;
  assert(bry_bridge_send(&bridge,page_request,sizeof(page_request),SCREEN_CAP_PAGE_CONTROL));
  assert(screen.model.page==0&&page_status[2]==2);
  page_request[8]=1;page_status[2]=99;
  assert(bry_bridge_send(&bridge,page_request,sizeof(page_request),SCREEN_CAP_PAGE_CONTROL));
  assert(page_status[2]==99); // Malformed direction request has no side effect/ACK.
  /* Reproduce an ESP-only reboot with the initial reset line dropped. The MCU
   * retains a ready session until its next valid report prompts negotiation. */
  screen.model.ota=false;
  unsigned reports=backlight_reports;
  bry_bridge_reset(&bridge);
  assert(screen.caps.ready&&screen.offered);
  assert(screen_runtime_backlight(&screen,60,60,0,true));
  assert(!screen.caps.ready&&!screen.offered&&!bridge.extended);
  assert(backlight_reports==reports);
  assert(screen_runtime_offer(&screen,0x56789abc));
  assert(screen.caps.ready&&bridge.extended&&bridge.session==0x56789abc);
  assert(screen_runtime_backlight(&screen,60,60,0,true));
  assert(backlight_reports==reports+1);
  scene_put32(clock+1,1793880060);
  assert(bry_bridge_send(&bridge,clock,sizeof(clock),SCREEN_CAP_SERVICES));
  assert(screen.clock_epoch==1793880060);
  puts("ESP to MCU: offer, ACK replay, atomic scenes, data, clock, "
       "reminders/DND, backlight telemetry, negotiated PD12 control/expiry, APP1 ACK/rejection, wallpaper query, failed TX, reboot and TTL passed");
}
