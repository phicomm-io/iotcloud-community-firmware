"""Extract the production startup preflight and its call before construction."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from DAIKIN.tests.arduinojson import codec_source

ROOT = Path(__file__).resolve().parents[1]

FIXTURE = r'''
#include "hub_home_budget.h"
#include <ArduinoJson.h>
#include <cassert>
#include <cstdio>
#include <string>
using namespace ArduinoJson;
// Object payload sizes read from the existing Xtensa ELF's DWARF. Production
// uses sizeof directly; pointer vectors here follow the host ABI naturally.
struct SpanAccessory { char bytes[20]; };
struct SpanCharacteristic { char bytes[128]; };
struct SpanService { char bytes[76]; };
struct HomeSensor { char bytes[100]; };
struct HomeOutlet { char bytes[88]; };
struct HomeFan { char bytes[88]; };
struct HomeLight { char bytes[88]; };
namespace Service { struct AccessoryInformation { char bytes[76]; }; }
namespace iotcloud { struct HubService { static constexpr size_t Devices = 12; }; }
static DynamicJsonDocument config(16384);
static std::string deviceId = "aabbccddeeff", homeConfiguration;
static size_t freeByte = 70000, largestByte = 24000;
static const unsigned MALLOC_CAP_INTERNAL = 1, MALLOC_CAP_8BIT = 2;
static size_t heap_caps_get_free_size(unsigned caps) {
  assert(caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)); return freeByte;
}
static size_t heap_caps_get_largest_free_block(unsigned caps) {
  assert(caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)); return largestByte;
}
static bool enabled = true;
static const char *homeError = "disabled";
static size_t construction = 0;
static bool homeEnabled() { return enabled; }
static std::string homeSignature() { return "unchanged-config"; }
#include "memory-ready.inc"
#include "home-init-prefix.inc"

static void clear() {
  config.clear(); config["hub"]["username"] = "owner";
  config["hub"].createNestedArray("devices");
  freeByte = 70000; largestByte = 24000; enabled = true;
  homeError = "disabled"; construction = 0;
}
static void add(const char *model, unsigned id) {
  auto d = config["hub"]["devices"].createNestedObject();
  char mac[13]; std::snprintf(mac, sizeof(mac), "%012x", id);
  d["mac"] = mac; d["model"] = model; d["version"] = "v2";
  d["status"] = "active"; d["username"] = "owner";
}
static size_t minimum() {
  size_t low = 0, high = 200000;
  largestByte = 24000;
  freeByte = high; assert(homeMemoryReady());
  while (low < high) {
    const auto middle = low + (high - low) / 2;
    freeByte = middle;
    if (homeMemoryReady()) high = middle; else low = middle + 1;
  }
  freeByte = low; return low;
}
int main() {
  using B = iotcloud::HomeGraphBudget;
  const B::Sizes target{20, 128, 76, 100, 88, 88, 88, 4};
  B graph(target);
  assert(graph.device(B::Daikin) && graph.device(B::M1) && graph.device(B::TC1) && graph.device(B::A1));
  assert(graph.accessories == 5 && graph.services == 22 && graph.characteristics == 78);
  assert(graph.fits(70000, 24000));
  const auto needed = graph.bytes;
  assert(needed > B::BusinessReserve + B::Bootstrap);
  assert(!graph.fits(needed - 1, B::MinimumBlock));
  assert(graph.fits(needed, B::MinimumBlock));
  assert(graph.bytes == needed && !graph.fits(200000, B::MinimumBlock - 1));

  B single(target), strip(target);
  assert(B::kind("s30-v1b") == B::S30V1B && B::kind("s30") == B::Invalid);
  assert(single.device(B::S30V1B) && strip.device(B::TC1));
  assert(single.accessories == 2 && single.services == 3 &&
         single.characteristics == 14 && single.loops == 1);
  assert(strip.accessories == 2 && strip.services == 8 &&
         strip.characteristics == 24 && strip.loops == 6);
  assert(single.bytes < strip.bytes && single.fits(70000, 24000));
  assert(!single.device(B::S30V1B)); // Finished budgets cannot change the graph.

  clear(); add("s30-v1b", 1);
  const auto singleBoundary = minimum();
  freeByte = singleBoundary - 1; assert(!homeMemoryReady());
  freeByte = singleBoundary; assert(homeMemoryReady());

  clear(); add("m1", 1); add("tc1", 2); add("a1", 3);
  assert(homeMemoryReady()); // Existing three-model + own-device baseline.
  const auto boundary = minimum();
  freeByte = boundary - 1;
  homeInit(); assert(!construction && !std::strcmp(homeError, "busy"));
  freeByte = boundary;
  homeInit(); assert(construction == 1 && !std::strcmp(homeError, "storage_error"));
  largestByte = B::MinimumBlock - 1; freeByte = 200000; construction = 0;
  homeInit(); assert(!construction && !std::strcmp(homeError, "busy"));
  enabled = false; homeInit(); assert(!construction);

  for (const auto *model : {"m1", "tc1", "daikin", "s30-v1b"}) {
    clear();
    for (unsigned i = 1; i <= 12; ++i) add(model, i);
    assert(!homeMemoryReady()); // Legal registration capacity, insufficient heap.
    homeInit(); assert(!construction && !std::strcmp(homeError, "busy"));
    freeByte = 200000; assert(homeMemoryReady());
    add(model, 13); assert(!homeMemoryReady()); // Invalid list rejected first.
  }

  clear();
  const auto ownOnly = minimum();
  add("daikin", 1); config["hub"]["devices"][0]["mac"] = deviceId;
  assert(minimum() == ownOnly); // Existing own-MAC suppression is preserved.
  add("m1", 2); config["hub"]["devices"][1]["mac"] = deviceId;
  freeByte = 200000; assert(!homeMemoryReady());
  clear(); add("unknown", 1); assert(!homeMemoryReady());
  clear(); add("m1", 1); config["hub"]["devices"][0]["status"] = "inactive";
  assert(!homeMemoryReady());
  clear(); add("m1", 1); config["hub"]["devices"][0]["username"] = "other";
  assert(!homeMemoryReady());
  clear(); add("m1", 1); config["hub"]["devices"][0]["mac"] = "000000000000";
  assert(!homeMemoryReady());
  clear(); config["hub"]["username"] = std::string(33, 'x'); assert(!homeMemoryReady());

  for (unsigned field = 0; field < 8; ++field) {
    auto huge = target;
    size_t *members[] = {&huge.accessory, &huge.characteristic, &huge.information, &huge.sensor,
                        &huge.outlet, &huge.fan, &huge.light, &huge.pointer};
    *members[field] = size_t(-1);
    B overflow(huge);
    overflow.device(B::Daikin); overflow.device(B::TC1); overflow.device(B::A1); overflow.device(B::S30V1B);
    assert(!overflow.fits(size_t(-1), size_t(-1)));
  }
  std::printf("HAP startup graph budget/preflight passed (target baseline need %zu bytes)\n", needed);
}
'''


class HapBudget(unittest.TestCase):
    def test_actual_preflight_and_initialization_gate(self):
        source = (ROOT / 'src/home.inc').read_text()
        method = 'static bool homeMemoryReady(){' + source.split('static bool homeMemoryReady(){', 1)[1].split(
            'static void homeInit(){', 1)[0]
        prefix = 'static void homeInit(){' + source.split('static void homeInit(){', 1)[1].split(
            'if(!homeStorage.open(true))', 1)[0]
        self.assertNotIn('new ', prefix)
        with tempfile.TemporaryDirectory(prefix='hap-startup-budget-') as temporary:
            build = Path(temporary)
            (build / 'memory-ready.inc').write_text(method)
            (build / 'home-init-prefix.inc').write_text(prefix + '\n++construction;\n}\n')
            fixture = build / 'budget.cpp'
            fixture.write_text(FIXTURE)
            binary = build / 'budget'
            subprocess.run([
                'c++', '-std=c++11', '-g', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie',
                '-I' + str(ROOT / 'include'), '-I' + str(build),
                '-I' + str(codec_source()),
                str(fixture), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=15,
                           env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1'})


if __name__ == '__main__':
    unittest.main()
