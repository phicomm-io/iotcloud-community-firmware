#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <new>
// A few scenarios cap actual C++ allocations as well as the production
// admission predicate. This catches hidden container growth after a guard.
static size_t allocationLimit = size_t(-1), largestAllocation = 0, deniedAllocations = 0;
void *operator new(std::size_t n) {
  largestAllocation = std::max(largestAllocation, n);
  if (n > allocationLimit) { ++deniedAllocations; throw std::bad_alloc(); }
  if (void *p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void *operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
#include "hub_transport.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <sys/time.h>

using iotcloud::HubBroker;
using iotcloud::HubTransport;
using Bytes = std::vector<uint8_t>;
static void text(Bytes &out, const std::string &s) {
  out.push_back(s.size() >> 8); out.push_back(s.size() & 255);
  out.insert(out.end(), s.begin(), s.end());
}
static Bytes frame(uint8_t flags, const Bytes &body) {
  Bytes out{flags}; size_t n = body.size();
  do { out.push_back(n % 128 | (n >= 128 ? 128 : 0)); n /= 128; } while (n);
  out.insert(out.end(), body.begin(), body.end()); return out;
}
static Bytes connectPacket() {
  Bytes b; text(b, "MQTT"); b.insert(b.end(), {4, 0xc2, 0, 30});
  text(b, "memory-review"); text(b, "owner"); text(b, "password");
  return frame(0x10, b);
}
static Bytes subscribePacket() {
  Bytes b{0, 1}; text(b, "topic/#"); b.push_back(0); return frame(0x82, b);
}
static Bytes publishPacket(const std::string &payload) {
  Bytes b; text(b, "topic/a"); b.insert(b.end(), payload.begin(), payload.end());
  return frame(0x30, b);
}
static void credentials(HubBroker &b) {
  b.authenticate = [](const std::string &, const std::string &) { return true; };
  b.authorize = [](const std::string &, bool) { return true; };
}
static void coalesced() {
  for (size_t chunk : {size_t(1), size_t(2), size_t(127), size_t(512), size_t(8197), size_t(20000)}) {
    HubBroker b; credentials(b); unsigned publications = 0; std::vector<Bytes> sent;
    b.send = [&](unsigned, const uint8_t *p, size_t n) { sent.emplace_back(p, p + n); return true; };
    b.event = [&](const std::string &topic, const std::string &payload, bool, HubBroker::Origin) {
      assert(topic == "topic/a" && payload == std::string(publications ? 8181 : 1, 'x'));
      ++publications;
    };
    assert(b.open(0, 0)); Bytes combined = connectPacket();
    for (const auto &packet : {subscribePacket(), publishPacket("x"),
                              publishPacket(std::string(8181, 'x')), frame(0xc0, {})})
      combined.insert(combined.end(), packet.begin(), packet.end());
    for (size_t at = 0; at < combined.size(); at += chunk)
      assert(b.receive(0, combined.data() + at, std::min(chunk, combined.size() - at), 1));
    assert(publications == 2 && sent.size() == 5 && sent.back() == Bytes({0xd0, 0}));
  }
}
static void callbackReset() {
  for (bool resetAll : {false, true}) {
    HubBroker b; credentials(b); unsigned calls = 0;
    b.send = [](unsigned, const uint8_t *, size_t) { return true; };
    assert(b.open(0, 0)); auto connect = connectPacket();
    assert(b.receive(0, connect.data(), connect.size(), 0));
    b.event = [&](const std::string &topic, const std::string &payload, bool, HubBroker::Origin) {
      ++calls;
      if (resetAll) b.reset(); else b.disconnect(0, true);
      // The command callback owns these bytes even after clearing its client.
      assert(topic == "topic/a" && payload == std::string(8000, 'z'));
    };
    auto input = publishPacket(std::string(8000, 'z'));
    auto ignored = publishPacket("must not execute after reset");
    input.insert(input.end(), ignored.begin(), ignored.end());
    assert(!b.receive(0, input.data(), input.size(), 1) && calls == 1 && !b.active(0));
  }
}
static void receiveGrowth() {
  HubBroker b; credentials(b);
  b.send = [](unsigned, const uint8_t *, size_t) { return true; };
  assert(b.open(0, 0)); auto connect = connectPacket();
  assert(b.receive(0, connect.data(), connect.size(), 0));
  // The parser accepts four-byte remaining lengths. Its maximum input buffer
  // can therefore need 8197 bytes, even when the last fragment contains one.
  Bytes input{0x30, 0x80, 0xc0, 0x80, 0x00}, body;
  text(body, "topic/a"); body.insert(body.end(), 8183, 'p'); assert(body.size() == 8192);
  input.insert(input.end(), body.begin(), body.end()); assert(input.size() == 8197);
  assert(b.receive(0, input.data(), input.size() - 1, 0));
  size_t checked = 0;
  b.allocate = [&](size_t n) { checked = n; return size_t(40000) > n + 16384 && size_t(8196) > n; };
  assert(!b.receive(0, input.data() + input.size() - 1, 1, 0));
  assert(checked == 8197 && !b.active(0));
}
struct AllocationCap {
  explicit AllocationCap(size_t n) { allocationLimit = n; largestAllocation = deniedAllocations = 0; }
  ~AllocationCap() { allocationLimit = size_t(-1); }
};
static void retainedGrowth() {
  HubBroker b; credentials(b);
  b.sendPublish = [](unsigned, const uint8_t *, size_t, const std::string &, const std::string &) { return true; };
  size_t checked = 0;
  b.allocate = [&](size_t n) { checked = std::max(checked, n); return n < 2548 && n + 16384 < 40000; };
  // A vector used to allocate a complete 64-element array at entry 33 even
  // though the tiny payload passed a 517-byte guard. Nodes remain small.
  for (unsigned i = 0; i < 32; ++i)
    assert(b.publish("t/" + std::to_string(i), "x", true));
  {
    AllocationCap cap(2548);
    for (unsigned i = 32; i < 128; ++i)
      assert(b.publish("t/" + std::to_string(i), "x", true));
    assert(b.retainedCount() == 128 && !b.publish("overflow", "x", true));
    assert(b.publish("t/0", "replacement", true));
    assert(b.publish("t/1", "", true) && b.retainedCount() == 127);
    assert(b.publish("replacement", "x", true) && b.retainedCount() == 128);
    assert(checked < 2548 && largestAllocation < 2548 && !deniedAllocations);
  }
}
static void smallReceive() {
  HubBroker b; credentials(b); unsigned publications = 0;
  b.send = [](unsigned, const uint8_t *, size_t) { return true; };
  b.sendPublish = [](unsigned, const uint8_t *, size_t, const std::string &, const std::string &) { return true; };
  const auto connect = connectPacket(), subscribe = subscribePacket();
  Bytes data = publishPacket(std::string(1024, 'x'));
  const auto second = publishPacket(std::string(1024, 'y'));
  data.insert(data.end(), second.begin(), second.end());
  size_t checked = 0;
  b.allocate = [&](size_t n) { checked = std::max(checked, n); return n < 2548 && n + 16384 < 40000; };
  b.event = [&](const std::string &topic, const std::string &payload, bool, HubBroker::Origin) {
    assert(topic == "topic/a" && payload == std::string(1024, publications ? 'y' : 'x'));
    ++publications;
  };
  {
    AllocationCap cap(2548);
    assert(b.open(0, 0));
    assert(b.receive(0, connect.data(), connect.size(), 0));
    assert(b.receive(0, subscribe.data(), subscribe.size(), 0));
    // The real transport reads at most 512 bytes at a time. Include a
    // coalesced tail belonging to the following publication in those chunks.
    for (size_t at = 0; at < data.size(); at += 512)
      assert(b.receive(0, data.data() + at, std::min(size_t(512), data.size() - at), 1));
    assert(publications == 2 && b.connected(0) && checked < 2548 && !deniedAllocations);
  }
}
static void receiveCopy() {
  HubBroker b; credentials(b); unsigned publications = 0;
  b.send = [](unsigned, const uint8_t *, size_t) { return true; };
  b.event = [&](const std::string &, const std::string &, bool, HubBroker::Origin) { ++publications; };
  assert(b.open(0, 0)); auto connect = connectPacket();
  assert(b.receive(0, connect.data(), connect.size(), 0));
  auto input = publishPacket(std::string(1400, 'x')); assert(input.size() < 1500);
  size_t checked = 0;
  b.allocate = [&](size_t n) { checked = n; return n < 1500; };
  AllocationCap cap(1500);
  // Receiving the packet fits, but its separate parsed-data admission fails
  // before packet copies or synchronous callbacks can consume the reserve.
  assert(!b.receive(0, input.data(), input.size(), 1));
  assert(checked > 1500 && !publications && !b.active(0) && !deniedAllocations);
}
static void duplicateSubscriptions() {
  HubBroker b; credentials(b); Bytes reply;
  b.send = [&](unsigned, const uint8_t *p, size_t n) { reply.assign(p, p + n); return true; };
  assert(b.open(0, 0)); auto connect = connectPacket();
  assert(b.receive(0, connect.data(), connect.size(), 0));
  Bytes body{0, 1};
  for (unsigned i = 0; i < 2047; ++i) { text(body, "t"); body.push_back(0); }
  const auto input = frame(0x82, body); assert(body.size() == 8190);
  b.allocate = [](size_t n) { return n < 9000; };
  {
    AllocationCap cap(9000);
    for (size_t at = 0; at < input.size(); at += 512)
      assert(b.receive(0, input.data() + at, std::min(size_t(512), input.size() - at), 1));
    // MQTT still requires one acknowledgement per requested filter. Replay
    // bookkeeping needs only the single unique subscription, not 2047 copies.
    assert(reply.size() == 2052 && reply[0] == 0x90 && reply[3] == 0 && reply[4] == 1);
    for (size_t i = 5; i < reply.size(); ++i) assert(reply[i] == 0);
    assert(b.connected(0) && !deniedAllocations);
  }
}
struct TransportFixture {
  HubTransport transport;
  int client = -1;
  TransportFixture(uint16_t tcp, uint16_t ws) {
    credentials(transport.broker); assert(transport.start(tcp, ws));
    client = ::socket(AF_INET, SOCK_STREAM, 0); assert(client >= 0);
    timeval timeout{2, 0}; assert(!setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)));
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(tcp);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(!::connect(client, reinterpret_cast<sockaddr *>(&address), sizeof(address)));
    transport.poll(0);
    auto input = connectPacket(), subscribe = subscribePacket();
    input.insert(input.end(), subscribe.begin(), subscribe.end());
    assert(::send(client, input.data(), input.size(), 0) == ssize_t(input.size()));
    transport.poll(1);
    assert(read(9) == Bytes({0x20, 2, 0, 0, 0x90, 3, 0, 1, 0}));
    assert(transport.broker.connected(0));
  }
  ~TransportFixture() { if (client >= 0) ::close(client); }
  Bytes read(size_t n) {
    Bytes result(n); size_t used = 0;
    while (used < n) {
      ssize_t got = ::recv(client, result.data() + used, n - used, 0);
      assert(got > 0); used += got;
    }
    return result;
  }
};
static void transmitGrowth(uint16_t tcp, uint16_t ws, bool direct) {
  TransportFixture f(tcp, ws); const std::string payload(8000, 'p');
  const Bytes expected = publishPacket(payload); assert(expected.size() == 8012);
  size_t largestChecked = 0;
  f.transport.broker.allocate = [&](size_t n) {
    largestChecked = std::max(largestChecked, n);
    return size_t(40000) > n + 16384 && size_t(14000) > n;
  };
  if (direct) {
    assert(f.transport.broker.publish("topic/a", payload));
    assert(f.transport.broker.publish("topic/a", payload));
    assert(!f.transport.broker.active(0));
  } else {
    assert(f.transport.broker.send(0, expected.data(), expected.size()));
    assert(!f.transport.broker.send(0, expected.data(), expected.size()));
    assert(f.transport.broker.connected(0));
  }
  // Reject the full replacement allocation before adding any new packet bytes.
  assert(largestChecked == 16024);
  f.transport.poll(2); assert(f.read(expected.size()) == expected);
  if (direct) { uint8_t extra; assert(::recv(f.client, &extra, 1, 0) == 0); }
}
int main(int argc, char **argv) {
  assert(argc == 4); const std::string scenario(argv[1]);
  if (scenario == "coalesced") coalesced();
  else if (scenario == "callback-reset") callbackReset();
  else if (scenario == "receive-growth") receiveGrowth();
  else if (scenario == "retained-growth") retainedGrowth();
  else if (scenario == "small-receive") smallReceive();
  else if (scenario == "receive-copy") receiveCopy();
  else if (scenario == "duplicate-subscriptions") duplicateSubscriptions();
  else {
    assert(scenario == "direct-growth" || scenario == "raw-growth");
    transmitGrowth(atoi(argv[2]), atoi(argv[3]), scenario == "direct-growth");
  }
  printf("PASS %s\n", argv[1]);
}
