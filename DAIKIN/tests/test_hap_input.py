"""Actual HAP parser growth and Span byte-heap admission under ASan/UBSan."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HOME = ROOT / 'lib/HomeSpan/src'

ADMISSION = r'''
#include "hub_home_http.h"
#include <cassert>
#include <cstdio>
#include <list>
#include <vector>

static size_t freeByte, largestByte;
static unsigned admins, stopped;
static const unsigned MALLOC_CAP_INTERNAL = 1, MALLOC_CAP_8BIT = 2;
static size_t heap_caps_get_free_size(unsigned caps) {
  assert(caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)); return freeByte;
}
static size_t heap_caps_get_largest_free_block(unsigned caps) {
  assert(caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)); return largestByte;
}
static const int WL_CONNECTED = 1, LWIP_SOCKET_OFFSET = 100;
static uint32_t millis() { return 123; }
struct { int status() { return WL_CONNECTED; } } WiFi;
struct Client {
  bool live = true;
  bool connected() const { return live; }
  void stop() { ++stopped; live = false; }
  void setTimeout(unsigned) {}
  int fd() const { return LWIP_SOCKET_OFFSET + 1; }
};
struct Server {
  bool hasClient() const { return true; }
  Client available() { return {}; }
};
struct HAPClient {
  iotcloud::HomeRequest request;
  iotcloud::HomeRecord encrypted;
  iotcloud::HomeInputWindow inputWindow;
  Client client;
  int clientNumber = 0;
  static unsigned nAdminControllers() { return admins; }
  static void checkNotifications() {}
  static void checkTimedWrites() {}
  void processRequest() {}
};
struct Service { void loop() {} };
struct Span {
  Server server;
  Server *hapServer = &server;
  std::list<HAPClient> hapList;
  std::list<HAPClient>::iterator currentClient;
  std::vector<Service *> Loops;
  uint32_t snapTime = 0;
  bool iotcloudInitialize() { return true; }
  bool iotcloudStorageHealthy() { return true; }
  void iotcloudStop() { assert(false); }
  void checkConnect() {}
  void clearNotify(HAPClient *) {}
  void pollTask();
};
#include "poll-task.inc"

static void admission(bool paired, size_t available, size_t block,
                      size_t clients, bool accepted) {
  admins = paired; freeByte = available; largestByte = block; stopped = 0;
  Span span;
  span.hapList.resize(clients);
  span.pollTask();
  assert(span.hapList.size() == clients + accepted);
  assert(stopped == unsigned(!accepted));
  if (!accepted) return;
  auto &client = span.hapList.front();
  assert(client.request.allocate && client.request.allocate == client.encrypted.allocate);
  assert(client.clientNumber == 1);
  const size_t reservation = 8097;
  freeByte = reservation + 16384; largestByte = reservation + 1;
  assert(!client.request.allocate(reservation));
  ++freeByte; largestByte = reservation;
  assert(!client.request.allocate(reservation));
  ++largestByte;
  assert(client.request.allocate(reservation));
  // Already admitted clients must still decline a growth after fragmentation.
  freeByte = 50000; largestByte = 32;
  const std::string request = "GET /accessories HTTP/1.1\r\n\r\n";
  assert(!client.request.append(reinterpret_cast<const uint8_t *>(request.data()), request.size()));
  assert(client.request.bytes.empty() && !client.request.complete());
  uint8_t record[] = {0, 4};
  largestByte = 31;
  assert(!client.encrypted.append(record, sizeof(record)));
  assert(client.encrypted.bytes.empty() && !client.encrypted.complete());
}
int main() {
  admission(true, 32767, 8192, 0, false);
  admission(true, 32768, 8191, 0, false);
  admission(true, 32768, 8192, 0, true);
  admission(false, 40959, 20480, 0, false);
  admission(false, 40960, 20479, 0, false);
  admission(false, 40960, 20480, 0, true);
  admission(true, 1000000, 1000000, 4, false);
  admission(false, 1000000, 1000000, 4, false);
  admission(true, 32768, 8192, 3, true);
  puts("HAP actual Span byte-heap admission and live parser callbacks passed");
}
'''


class HapInput(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='hap-input-memory-')
        cls.addClassCleanup(cls.temp.cleanup)
        cls.build = Path(cls.temp.name)
        source = (HOME / 'HomeSpan.cpp').read_text()
        method = 'void Span::pollTask(){' + source.split('void Span::pollTask(){', 1)[1].split(
            'void Span::commandMode()', 1)[0]
        (cls.build / 'poll-task.inc').write_text(method)
        fixture = cls.build / 'admission.cpp'
        fixture.write_text(ADMISSION)
        for name, path in (('http', ROOT / 'tests/hub/http.cpp'), ('admission', fixture)):
            subprocess.run([
                'c++', '-std=c++11', '-g', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie',
                '-I' + str(ROOT / 'include'), '-I' + str(cls.build),
                str(path), '-o', str(cls.build / name)], check=True)

    def scenario(self, name):
        subprocess.run([str(self.build / name)], check=True, timeout=20,
                       env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1'})

    def test_actual_http_and_record_growth_with_full_replacement_budget(self):
        self.scenario('http')

    def test_actual_span_uses_byte_heap_thresholds_and_live_allocation_checks(self):
        self.scenario('admission')


if __name__ == '__main__':
    unittest.main()
