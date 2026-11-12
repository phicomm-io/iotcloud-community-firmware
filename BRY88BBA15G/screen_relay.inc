/* One bounded socket worker. It never parses HTTP/JPEG and never writes UART.
 * A single MCU credit permits at most 512 TCP bytes to leave this worker. */
#include <freertos/queue.h>
struct ScreenRelayItem {
  uint32_t connection = 0;
  uint16_t size = 0;
  uint8_t op = 0;
  uint8_t bytes[MEDIA_CHUNK] = {};
};
static QueueHandle_t screenRelayJobs = nullptr, screenRelayResults = nullptr;
static TaskHandle_t screenRelayTask = nullptr;
static struct {
  uint32_t connection = 0, sequence = 0, pendingSequence = 0, sent = 0,
           lastEvent = 0, lastSignature = 0, closing = 0;
  unsigned attempts = 0;
  bool pending = false;
  ScreenRelayItem item;
} screenRelay;
static void screenRelayWorker(void *) {
  WiFiClient plain;
  WiFiClientSecure secure;
  Client *socket = nullptr;
  uint32_t connection = 0;
  bool credit = false;
  auto report = [&](unsigned op, const uint8_t *data = nullptr, size_t n = 0) {
    ScreenRelayItem result;
    result.op = op;
    result.connection = connection;
    result.size = n;
    if (n)
      memcpy(result.bytes, data, n);
    return xQueueSend(screenRelayResults, &result, pdMS_TO_TICKS(100)) ==
           pdTRUE;
  };
  for (;;) {
    ScreenRelayItem job;
    if (xQueueReceive(screenRelayJobs, &job, pdMS_TO_TICKS(10)) == pdTRUE) {
      if (job.op == MEDIA_NET_OPEN) {
        plain.stop();
        secure.stop();
        socket = nullptr;
        credit = false;
        connection = job.connection;
        bool tls = job.bytes[0], verify = job.bytes[1];
        unsigned port = job.bytes[2] | job.bytes[3] << 8;
        const char *host = (const char *)job.bytes + 4;
        int connected = 0;
        if (tls) {
          if (verify)
            secure.setCACertBundle(weatherCaBundle);
          else
            secure.setInsecure();
          secure.setHandshakeTimeout(5);
          secure.setTimeout(1000);
          connected = secure.connect(host, port, 5000);
          if (connected)
            socket = &secure;
        } else {
          plain.setTimeout(1000);
          connected = plain.connect(host, port, 5000);
          if (connected)
            socket = &plain;
        }
        report(connected ? MEDIA_NET_CONNECTED : MEDIA_NET_ERROR);
      } else if (job.connection == connection) {
        if (job.op == MEDIA_NET_CLOSE) {
          plain.stop();
          secure.stop();
          socket = nullptr;
          credit = false;
        } else if (job.op == MEDIA_SOCKET_ACK)
          credit = true;
        else if (job.op == MEDIA_NET_WRITE && socket) {
          if (socket->write(job.bytes, job.size) != job.size) {
            report(MEDIA_NET_ERROR);
            socket->stop();
            socket = nullptr;
            credit = false;
          }
        }
      }
    }
    if (socket && credit) {
      int available = socket->available();
      if (available > 0) {
        ScreenRelayItem result;
        result.connection = connection;
        result.op = MEDIA_NET_WRITE;
        int n = socket->read(result.bytes, min(available, int(MEDIA_CHUNK)));
        if (n > 0) {
          result.size = n;
          credit = false;
          if (xQueueSend(screenRelayResults, &result, pdMS_TO_TICKS(100)) !=
              pdTRUE) {
            socket->stop();
            socket = nullptr;
          }
        }
      } else if (!socket->connected()) {
        report(MEDIA_NET_CLOSED);
        socket->stop();
        socket = nullptr;
        credit = false;
      }
    }
  }
}
static bool screenRelayInit() {
  if (screenRelayTask)
    return true;
  if (ESP.getFreeHeap() < 49152)
    return false;
  if (!screenRelayJobs)
    screenRelayJobs = xQueueCreate(4, sizeof(ScreenRelayItem));
  if (!screenRelayResults)
    screenRelayResults = xQueueCreate(2, sizeof(ScreenRelayItem));
  return screenRelayJobs && screenRelayResults &&
         xTaskCreate(screenRelayWorker, "screen_tcp", 8192, nullptr, 1,
                     &screenRelayTask) == pdPASS;
}
static bool screenRelayJob(unsigned op, uint32_t connection,
                           const void *p = nullptr, size_t n = 0) {
  if (!screenRelayJobs || n > MEDIA_CHUNK)
    return false;
  ScreenRelayItem job;
  job.op = op;
  job.connection = connection;
  job.size = n;
  if (n)
    memcpy(job.bytes, p, n);
  return xQueueSend(screenRelayJobs, &job, 0) == pdTRUE;
}
static void screenRelayStop() {
  if (screenRelay.connection)
    screenRelay.closing = screenRelay.connection;
  if (screenRelay.closing &&
      screenRelayJob(MEDIA_NET_CLOSE, screenRelay.closing))
    screenRelay.closing = 0;
  screenRelay.connection = 0;
  screenRelay.pending = false;
  screenRelay.lastEvent = 0;
  screenRelay.lastSignature = 0;
}
static void screenRelayPacket(unsigned channel, uint32_t sequence,
                              const uint8_t *p, size_t n) {
  if (n < 5)
    return;
  uint32_t connection = media_u32(p + 1);
  if (channel == MEDIA_SOCKET_ACK) {
    if (screenRelay.pending && connection == screenRelay.connection &&
        sequence == screenRelay.pendingSequence) {
      if (screenRelayJob(MEDIA_SOCKET_ACK, connection)) {
        screenRelay.pending = false;
      }
    }
    return;
  }
  if (channel != MEDIA_SOCKET_EVENT)
    return;
  uint32_t signature = media_crc32(p, n);
  if (sequence == screenRelay.lastEvent &&
      signature == screenRelay.lastSignature) {
    screenMediaWrite(MEDIA_SOCKET_ACK, sequence, p, 5);
    return;
  }
  if (sequence <= screenRelay.lastEvent)
    return;
  bool accepted = false;
  if (p[0] == MEDIA_NET_OPEN && n >= 10 && n <= 141) {
    size_t hostSize = n - 9;
    if (p[5] > 1 || p[6] > 1 || !(p[7] | p[8] << 8) || !hostSize ||
        hostSize > 128 || p[n - 1] || !memchr(p + 9, 0, hostSize))
      return;
    if (!screenRelayInit()) {
      uint8_t error[5] = {MEDIA_NET_ERROR};
      media_put32(error + 1, connection);
      screenMediaWrite(MEDIA_SOCKET_EVENT, 0, error, sizeof(error));
      return;
    }
    if (screenRelayJob(MEDIA_NET_OPEN, connection, p + 5, n - 5)) {
      screenRelay.connection = connection;
      screenRelay.sequence = 0;
      screenRelay.pending = false;
      accepted = true;
    }
  } else if (connection == screenRelay.connection) {
    if (p[0] == MEDIA_NET_CLOSE && n == 5) {
      accepted = screenRelayJob(MEDIA_NET_CLOSE, connection);
      if (accepted) {
        screenRelay.connection = 0;
        screenRelay.pending = false;
      }
    } else if (p[0] == MEDIA_NET_WRITE && n > 5 && n <= MEDIA_CHUNK + 5)
      accepted = screenRelayJob(MEDIA_NET_WRITE, connection, p + 5, n - 5);
  }
  if (accepted) {
    screenRelay.lastEvent = sequence;
    screenRelay.lastSignature = signature;
    screenMediaWrite(MEDIA_SOCKET_ACK, sequence, p, 5);
  }
}
static void screenRelayPoll(uint32_t now) {
  if (screenRelay.closing &&
      screenRelayJob(MEDIA_NET_CLOSE, screenRelay.closing))
    screenRelay.closing = 0;
  if (screenOtaActive() || !screenMediaAvailable()) {
    screenRelayStop();
  }
  if (!screenRelayResults)
    return;
  if (!screenRelay.pending) {
    ScreenRelayItem item;
    if (xQueueReceive(screenRelayResults, &item, 0) == pdTRUE &&
        item.connection == screenRelay.connection) {
      if (item.op == MEDIA_NET_WRITE) {
        screenRelay.item = item;
        screenRelay.pending = true;
        screenRelay.pendingSequence = ++screenRelay.sequence;
        screenRelay.attempts = 0;
        screenRelay.sent = 0;
      } else {
        uint8_t event[5] = {item.op};
        media_put32(event + 1, item.connection);
        screenMediaWrite(MEDIA_SOCKET_EVENT, 0, event, sizeof(event));
        if (item.op == MEDIA_NET_CONNECTED)
          screenRelayJob(MEDIA_SOCKET_ACK, item.connection);
      }
    }
  }
  if (screenRelay.pending &&
      (!screenRelay.sent || uint32_t(now - screenRelay.sent) > 1500)) {
    if (screenRelay.attempts >= 3) {
      screenRelayStop();
      return;
    }
    uint8_t p[MEDIA_CHUNK + 5] = {MEDIA_NET_WRITE};
    media_put32(p + 1, screenRelay.connection);
    memcpy(p + 5, screenRelay.item.bytes, screenRelay.item.size);
    if (screenMediaWrite(MEDIA_SOCKET, screenRelay.pendingSequence, p,
                         screenRelay.item.size + 5)) {
      screenRelay.sent = now;
      screenRelay.attempts++;
    }
  }
}
