#include "hub_broker.h"
#include "generated_topics.h"
#include <cassert>
#include <cstdio>

using iotcloud::HubBroker;
using Bytes = std::vector<uint8_t>;
static void text(Bytes &out, const std::string &s) {
  out.push_back(s.size() >> 8); out.push_back(s.size() & 255);
  out.insert(out.end(), s.begin(), s.end());
}
static Bytes frame(uint8_t flags, const Bytes &body) {
  Bytes out{flags}; size_t n = body.size();
  do { uint8_t b = n % 128; n /= 128; out.push_back(b | (n ? 128 : 0)); } while (n);
  out.insert(out.end(), body.begin(), body.end()); return out;
}
static Bytes body(const Bytes &packet) {
  size_t at = 1; while (packet[at++] & 128) {};
  return Bytes(packet.begin() + at, packet.end());
}
struct Fixture {
  HubBroker broker;
  std::array<std::vector<Bytes>, HubBroker::Clients> sent;
  Fixture() {
    broker.authenticate = [](const std::string &, const std::string &) { return true; };
    broker.authorize = [](const std::string &, bool) { return true; };
    broker.send = [this](unsigned slot, const uint8_t *p, size_t n) {
      sent[slot].emplace_back(p, p + n); return true;
    };
  }
  bool input(unsigned slot, const Bytes &packet) {
    for (size_t at = 0; at < packet.size(); at += 137)
      if (!broker.receive(slot, packet.data() + at, std::min(size_t(137), packet.size() - at), 0)) return false;
    return true;
  }
  bool connect(unsigned slot, const std::string &will = {}, const std::string &payload = {}) {
    assert(broker.open(slot, 0));
    Bytes b; text(b, "MQTT");
    b.insert(b.end(), {4, uint8_t(will.empty() ? 0xc2 : 0xc6), 0, 30});
    text(b, std::string(127, 'i') + char('0' + slot));
    if (!will.empty()) { text(b, will); text(b, payload); }
    text(b, std::string(63, 'u')); text(b, std::string(128, 'p'));
    assert(b.size() <= HubBroker::Packet);
    if (will.size() == 512 && payload.size() == 1024) assert(b.size() == 1875);
    return input(slot, frame(0x10, b));
  }
  Bytes subscribe(unsigned slot, const std::vector<std::string> &filters) {
    Bytes b{0, 1}; for (const auto &s : filters) { text(b, s); b.push_back(1); }
    assert(input(slot, frame(0x82, b))); return body(sent[slot].back());
  }
  void unsubscribe(unsigned slot, const std::string &filter) {
    Bytes b{0, 2}; text(b, filter); assert(input(slot, frame(0xa2, b)));
  }
};
static std::string topic(unsigned id) {
  return std::to_string(id) + std::string(HubBroker::Topic - std::to_string(id).size(), 'x');
}
static void generated() {
  for (unsigned length : {32u, 63u}) {
    feature_config_t c{}; std::string user(length, 'a');
    memset(c.group, 'G', 32); c.member_count = 1; strcpy(c.members[0].id, "112233445566");
    assert(mqtt_account_init(&account, MQTT_TOPIC_TMCP, "a1", user.c_str(), LOCAL_ID));
    char root[FEATURE_TOPIC_MAX], safe[FEATURE_TOPIC_MAX], owner[13];
    char discovery_id[DiscoveryIdCapacity], parent_id[ParentIdCapacity], entity[240], path[FEATURE_TOPIC_MAX];
    root_for(&c, 0, root);
    assert(virtual_identity(c.group, c.members, c.member_count, root, &account, owner,
                            safe, sizeof safe, discovery_id, sizeof discovery_id));
    assert(virtual_identity(c.group, c.members, c.member_count, root, &account, owner,
                            safe, sizeof safe, parent_id, sizeof parent_id));
    assert(strlen(discovery_id) == (length == 32 ? 208u : 270u));
    assert(!strcmp(discovery_id, parent_id)); strcpy(account.mac, owner);
    assert(mqtt_format(entity, sizeof entity, "bound_%s_device_led_always_on", safe));
    assert(mqtt_ha(&account, "switch", entity, path, sizeof path));
    const std::string discovery(path); assert(discovery.size() == (length == 32 ? 270u : 332u));
    assert(mqtt_format(entity, sizeof entity, "bound_%s_%s_scd40_temperature", safe, c.members[0].id));
    assert(mqtt_ha(&account, "sensor", entity, path, sizeof path));
    const std::string tombstone(path); assert(tombstone.size() == (length == 32 ? 280u : 342u));
    Fixture f; assert(f.connect(0));
    assert(f.subscribe(0, {discovery, tombstone}) == Bytes({0, 1, 0, 0}));
    assert(f.broker.publish(discovery, "{\"device\":\"complete\"}", true));
    assert(f.broker.publish(tombstone, "old proxy", true));
    assert(f.broker.retainedCount() == 2);
    assert(f.connect(1)); f.subscribe(1, {discovery, tombstone}); f.broker.tick(0);
    assert(f.sent[1].size() == 4); // CONNACK, SUBACK and two retained deliveries.
    assert(f.sent[1][2][0] == 0x31 && f.sent[1][3][0] == 0x31);
    assert(f.broker.publish(tombstone, "", true)); assert(f.broker.retainedCount() == 1);
    Bytes expected; text(expected, tombstone); assert(body(f.sent[0].back()) == expected);
    printf("user=%u group=32 identity=%zu discovery=%zu tombstone=%zu\n", length,
           strlen(discovery_id), discovery.size(), tombstone.size());
  }
}
static void boundaries() {
  const auto exact = topic(0), tooLong = exact + "x";
  assert(HubBroker::topicValid(exact) && HubBroker::topicValid(exact, true));
  assert(!HubBroker::topicValid(tooLong) && !HubBroker::topicValid(tooLong, true));
  std::string unicode; for (unsigned i = 0; i < 256; ++i) unicode += "\xc3\xa9";
  assert(HubBroker::topicValid(unicode)); assert(!HubBroker::topicValid(unicode + "x"));
  Fixture f; assert(f.connect(0)); assert(f.subscribe(0, {exact}) == Bytes({0, 1, 0}));
  const std::string payload(HubBroker::Packet - exact.size() - 4, 'p');
  assert(payload.size() == 7676 && f.broker.publish(exact, payload));
  assert(!f.broker.publish(exact, payload + "x")); assert(!f.broker.publish(tooLong, ""));
  assert(f.connect(1));
  Bytes b; text(b, exact); b.insert(b.end(), {0, 7}); b.insert(b.end(), payload.begin(), payload.end());
  const auto packet = frame(0x32, b); assert(b.size() == 8192 && packet.size() == 8195);
  assert(f.input(1, packet)); assert(f.sent[1].back() == Bytes({0x40, 2, 0, 7}));
  assert(f.sent[0].back().size() == 8193); // QoS 0 delivery omits the packet id.
  b.push_back('x'); assert(!f.input(1, frame(0x32, b)));
  Fixture invalid; assert(invalid.connect(0)); b = {0, 1}; text(b, tooLong); b.push_back(0);
  assert(!invalid.input(0, frame(0x82, b)));
  Fixture will; assert(will.connect(0, exact, std::string(1024, 'w')));
  assert(!will.connect(1, tooLong, "w")); assert(body(will.sent[1].back()) == Bytes({0, 2}));
  assert(!will.connect(1, exact, std::string(1025, 'w')));
  assert(body(will.sent[1].back()) == Bytes({0, 2}));
  Fixture batch; assert(batch.connect(0)); assert(batch.connect(1));
  std::vector<std::string> filters; for (unsigned i = 0; i < 15; ++i) filters.push_back(topic(i));
  assert(batch.subscribe(0, filters).size() == 17); // Remaining length 7727 fits.
  b = {0, 1}; for (unsigned i = 0; i < 16; ++i) { text(b, topic(i)); b.push_back(1); }
  assert(b.size() == 8242 && !batch.input(1, frame(0x82, b)));
  assert(batch.broker.connected(0));
}
static void subscriptions() {
  Fixture f; assert(f.connect(0)); assert(f.connect(1));
  for (unsigned i = 0; i < 16; ++i) assert(f.subscribe(i % 2, {topic(i)}) == Bytes({0, 1, 0}));
  assert(f.subscribe(1, {topic(16)}) == Bytes({0, 1, 128}));
  assert(f.subscribe(0, {topic(0)}) == Bytes({0, 1, 0})); // Updating a full-budget filter is allowed.
  f.unsubscribe(0, topic(0)); assert(f.subscribe(1, {topic(16)}) == Bytes({0, 1, 0}));
  assert(f.subscribe(0, {"extra"}) == Bytes({0, 1, 128}));
}
static void retained() {
  Fixture f; const std::string payload(HubBroker::Packet - HubBroker::Topic - 4, 'r');
  for (unsigned i = 0; i < 6; ++i) assert(f.broker.publish(topic(i), payload, true));
  assert(f.broker.retainedBytes() == 49128);
  assert(f.broker.publish(std::string(23, 'z'), "r", true));
  assert(f.broker.retainedBytes() == HubBroker::RetainedBudget);
  assert(!f.broker.publish("new", "r", true));
  assert(f.broker.publish(topic(0), payload, true));
  assert(f.broker.publish(topic(0), "", true)); assert(f.broker.publish("new", "r", true));
  assert(f.broker.retainedBytes() <= HubBroker::RetainedBudget);
}
static void willAndReceive() {
  Fixture f;
  assert(f.connect(0, topic(0), std::string(1024, 'w')));
  assert(f.connect(1, topic(1), std::string(1024, 'w')));
  assert(f.connect(2, topic(2), std::string(512, 'w')));
  assert(!f.connect(3, "w", "")); assert(body(f.sent[3].back()) == Bytes({0, 3}));
  f.broker.disconnect(0, true); assert(f.connect(3, topic(3), std::string(1024, 'w')));
  Fixture receive; for (unsigned i = 0; i < 3; ++i) assert(receive.connect(i));
  Bytes partial = frame(0x30, Bytes(HubBroker::Packet)); partial.pop_back();
  assert(partial.size() == 8194);
  assert(receive.broker.receive(0, partial.data(), partial.size(), 0));
  assert(receive.broker.receive(1, partial.data(), partial.size(), 0));
  assert(!receive.broker.receive(2, partial.data(), partial.size(), 0));
  assert(receive.broker.connected(0) && receive.broker.connected(1));
}
int main(int argc, char **argv) {
  static_assert(HubBroker::Topic == 512 && HubBroker::Packet == 8192, "wire bounds");
  static_assert(HubBroker::ReceiveBudget == 24576 && HubBroker::RetainedBudget == 49152 &&
                HubBroker::WillBudget == 4096 && HubBroker::SubscriptionBudget == 8192, "shared storage bounds");
  assert(argc == 2); const std::string scenario(argv[1]);
  if (scenario == "generated") generated(); else if (scenario == "boundaries") boundaries();
  else if (scenario == "subscriptions") subscriptions(); else if (scenario == "retained") retained();
  else { assert(scenario == "will-receive"); willAndReceive(); }
  printf("PASS %s sizeof(HubBroker)=%zu\n", argv[1], sizeof(HubBroker));
}
