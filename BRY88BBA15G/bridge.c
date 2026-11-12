#include "bridge.h"
#include "protocol/widgets.h"
#include <string.h>
static void line(void *context, const char *p, size_t n) {
  bry_bridge *b = context;
  if (n >= 12 && !memcmp(p, "screen_caps ", 12)) {
    uint32_t session, caps;
    char response[80];
    if (!screen_caps_parse_offer(p, n, &session, &caps) || !b->send)
      return;
    if ((caps & (SCREEN_CAP_KNOWN & ~(SCREEN_CAP_FLASH_INFO | SCREEN_CAP_MEDIA | SCREEN_CAP_IMAGE | SCREEN_CAP_BINARY_LINK | SCREEN_CAP_URL_PUSH | SCREEN_CAP_PUSH_UPDATE | SCREEN_CAP_PUSH_POLICY))) != (SCREEN_CAP_KNOWN & ~(SCREEN_CAP_FLASH_INFO | SCREEN_CAP_MEDIA | SCREEN_CAP_IMAGE | SCREEN_CAP_BINARY_LINK | SCREEN_CAP_URL_PUSH | SCREEN_CAP_PUSH_UPDATE | SCREEN_CAP_PUSH_POLICY)))
      return; /* rc6 supports the current community MCU or factory UART only. */
    /* Repeat ACKs preserve the capabilities agreed for this session. */
    if (b->extended && session == b->session) caps = b->capabilities;
    caps &= SCREEN_CAP_WIDGETS | SCREEN_CAP_SCENE | SCREEN_CAP_SERVICES |
            SCREEN_CAP_BACKLIGHT | SCREEN_CAP_DISPLAY | SCREEN_CAP_SCENE_V3 |
            SCREEN_CAP_VALUE_MODE | SCREEN_CAP_RESPONSIVE |
            SCREEN_CAP_MODULE_CONTROL | SCREEN_CAP_APPEARANCE |
            SCREEN_CAP_WALLPAPER | SCREEN_CAP_MULTI_UNITS | SCREEN_CAP_HIDE_TITLE |
            SCREEN_CAP_FONT_BANK | SCREEN_CAP_LANGUAGE | SCREEN_CAP_CHART_POINTS | SCREEN_CAP_PAGE_RAIL | SCREEN_CAP_RESOURCE_IO | SCREEN_CAP_DISPLAY_DIAG | SCREEN_CAP_PAGE_CONTROL | SCREEN_CAP_COMPACT_WIDGETS | SCREEN_CAP_MEDIA | SCREEN_CAP_IMAGE | SCREEN_CAP_BINARY_LINK | SCREEN_CAP_URL_PUSH | SCREEN_CAP_PUSH_UPDATE | SCREEN_CAP_PUSH_POLICY;
    size_t used = screen_caps_ack(response, sizeof(response), session, caps);
    if (used && b->send(b->context, response, used)) {
      if (!b->extended || session != b->session) {
        b->last_sequence = 0;
        b->tx_sequence = 0;
      }
      b->extended = true;
      b->session = session;
      b->capabilities = caps;
    }
    return;
  }
  if (n >= 11 && !memcmp(p, "screen_ext ", 11)) {
    uint8_t payload[SCREEN_LINK_CHUNK];
    size_t length = sizeof(payload);
    uint32_t session, seq;
    if (!screen_link_decode(p, n, &session, &seq, payload, &length))
      return;
    if (!b->extended) {
      /* ESP-only OTA can leave the MCU in its previous session if the boot
       * reset line was lost. A valid MCU frame requests a fresh offer. */
      static const char reset[] = "\r\n+screen_link_reset:1\r\n";
      if (b->send)
        b->send(b->context, reset, sizeof(reset) - 1);
      return;
    }
    if (session != b->session || seq <= b->last_sequence)
      return;
    b->last_sequence = seq;
    if (b->extension)
      b->extension(b->context, payload, length);
    return;
  }
  /* A factory version exchange is also the boot/reconnect boundary. Preserve
   * its bytes for the stock command owner while discarding stale capabilities.
   */
  if (n == 8 && !memcmp(p, "version?", 8)) {
    b->extended = false;
    b->capabilities = 0;
    b->session = 0;
    b->last_sequence = 0;
    b->tx_sequence = 0;
  }
  if (b->factory)
    b->factory(b->context, p, n);
}
void bry_bridge_init(bry_bridge *b) { bry_bridge_reset(b); }
void bry_bridge_reset(bry_bridge *b) {
  b->extended = false;
  b->capabilities = 0;
  b->session = 0;
  b->last_sequence = 0;
  b->tx_sequence = 0;
  screen_link_init(&b->parser, line, b);
}
bool bry_bridge_negotiate(bry_bridge *b, unsigned schema, uint32_t session) {
  if (schema != SCREEN_LINK_SCHEMA || !session) {
    bry_bridge_reset(b);
    return false;
  }
  b->extended = true;
  b->capabilities = SCREEN_CAP_WIDGETS;
  b->session = session;
  b->last_sequence = 0;
  b->tx_sequence = 0;
  return true;
}
void bry_bridge_feed(bry_bridge *b, const void *p, size_t n) {
  screen_link_feed(&b->parser, p, n);
}

bool bry_bridge_publish(bry_bridge *b, const void *data, size_t n) {
  if (!b->extended || !(b->capabilities & SCREEN_CAP_WIDGETS) || !b->send ||
      b->tx_sequence == UINT32_MAX)
    return false;
  screen_widgets validated = {0};
  if (!screen_widget_apply(&validated, data, n, 0))
    return false;
  char line[SCREEN_LINK_LINE];
  size_t used = screen_link_encode(line, sizeof(line), b->session,
                                   b->tx_sequence + 1, data, n);
  if (!used || !b->send(b->context, line, used))
    return false;
  b->tx_sequence++;
  return true;
}

bool bry_bridge_send(bry_bridge *b, const void *data, size_t n,
                     uint32_t capability) {
  if (!b->extended || !(b->capabilities & capability) || !b->send || !data ||
      !n || n > SCREEN_LINK_CHUNK || b->tx_sequence == UINT32_MAX)
    return false;
  char wire[SCREEN_LINK_LINE];
  size_t size = screen_link_encode(wire, sizeof(wire), b->session,
                                   b->tx_sequence + 1, data, n);
  if (!size || !b->send(b->context, wire, size))
    return false;
  b->tx_sequence++;
  return true;
}
