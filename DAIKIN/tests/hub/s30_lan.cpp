/* Actual UDP to the BL2028N production endpoint host. The included hub gates,
 * start function and authorizer are extracted unchanged from hub_lan.inc. */
#include "hub_home.h"
#include "hub_lan_discovery.h"
#include "hub_lan_client.h"
#include "hub_lan_result.h"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netinet/in.h>
#include <openssl/rand.h>
extern "C" {
#include "local_auth.h"
}
#include "lan_client_core.inc"

static const auto began = std::chrono::steady_clock::now();
static uint32_t millis() {
  return 10001 + std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - began).count();
}
static int clientfd;
static sockaddr_in destination;
static std::string peerId, deviceId = "02c400000002";
static uint32_t hubLanBoot = 81;
static bool localAuthReady = true, otaActive = false, ready = true, heapReady = true;
static ph_auth_state localAuth;
static ph_auth_store persisted;
static lc_state lanClient;
static lc_ops lanOps;
static DynamicJsonDocument config(16384);
static iotcloud::HubService hubService(config);
static iotcloud::HubLanDiscovery hubLanDiscovery;
static iotcloud::HubLanClient hubLanClient;
static const char *hubLanWaitReason = "idle";
static bool hubLanRequestStatus;
static std::string hubLanRequestId;
static unsigned pairs, resumes, commands, pulls, acks, completed;
static std::string lastReply;
static void initLanClient() {}
static bool hubReady() { return ready; }
static bool hubAllocationReady(size_t) { return heapReady; }
#include "hub-s30-gates.inc"

static bool sendPacket(const void *bytes, size_t length) {
  ph_value root, op;
  assert(phv_parse(PH_WIRE_MSGPACK, bytes, length, &root) && phv_get(&root, "op", &op));
  if (phv_equal_text(&op, "auth_pair")) ++pairs;
  if (phv_equal_text(&op, "auth_resume")) ++resumes;
  if (phv_equal_text(&op, "command")) ++commands;
  if (phv_equal_text(&op, "pull")) ++pulls;
  if (phv_equal_text(&op, "ack")) ++acks;
  return sendto(clientfd, bytes, length, 0, reinterpret_cast<const sockaddr *>(&destination),
                sizeof(destination)) == ssize_t(length);
}
static void receive() {
  pollfd p{clientfd, POLLIN, 0};
  if (poll(&p, 1, 20) <= 0) return;
  char frame[LAN_V4_FRAME_MAX + 1];
  sockaddr_in source{};
  socklen_t sourceLength = sizeof(source);
  const auto n = recvfrom(clientfd, frame, LAN_V4_FRAME_MAX, 0,
                         reinterpret_cast<sockaddr *>(&source), &sourceLength);
  assert(n > 0 && source.sin_addr.s_addr == destination.sin_addr.s_addr &&
         source.sin_port == destination.sin_port);
  ph_value root;
  assert(phv_parse(PH_WIRE_MSGPACK, frame, size_t(n), &root));
  lan_v4_packet packet{};
  if (lan_v4_decode_value(&root, &packet)) {
    hubLanClient.receive(packet, "127.0.0.1", ntohs(destination.sin_port), millis());
  } else {
    assert(lc_response(&lanClient, &lanOps, &root, "127.0.0.1"));
  }
}
template<class Predicate> static void until(Predicate done) {
  const auto started = millis();
  while (!done()) {
    assert(uint32_t(millis() - started) < 10000);
    lc_tick(&lanClient, &lanOps);
    hubLanClient.tick(millis());
    receive();
  }
}
static std::string request(const char *action, const char *id, int index = -1) {
  DynamicJsonDocument doc(512);
  doc["action"] = action; doc["request_id"] = id;
  auto params = doc.createNestedObject("params");
  if (index >= 0) { params["index"] = index; params["on"] = false; }
  std::string body; serializeMsgPack(doc, body); return body;
}
static std::string businessCode() {
  DynamicJsonDocument doc(4096);
  assert(!deserializeMsgPack(doc, lastReply));
  return doc["code"] | "";
}

int main(int argc, char **argv) {
  assert(argc == 4);
  const long port = std::strtol(argv[1], nullptr, 10);
  assert(port > 0 && port <= 65535);
  peerId = argv[2];
  assert(lc_peer(peerId.c_str()) && peerId != deviceId);
  clientfd = socket(AF_INET, SOCK_DGRAM, 0); assert(clientfd >= 0);
  sockaddr_in source{}; source.sin_family = AF_INET;
  source.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  assert(!bind(clientfd, reinterpret_cast<sockaddr *>(&source), sizeof(source)));
  socklen_t sourceLength = sizeof(source);
  assert(!getsockname(clientfd, reinterpret_cast<sockaddr *>(&source), &sourceLength));
  assert(ntohs(source.sin_port) != LAN_V4_PORT); // Replies must use our ephemeral port.
  destination.sin_family = AF_INET; destination.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  destination.sin_port = htons(uint16_t(port));

  ph_auth_store_empty(&persisted);
  ph_auth_ops ops{};
  ops.now = [](void *) { return millis(); };
  ops.random = [](void *, uint8_t *p, size_t n) { return RAND_bytes(p, n) == 1; };
  ops.password = [](void *, char out[33]) { std::strcpy(out, "hub-host-password"); return true; };
  ops.save = [](void *, const ph_auth_store *store) { persisted = *store; return true; };
  assert(ph_auth_init(&localAuth, &ops, &persisted));
  lanOps.local = deviceId.c_str(); lanOps.now = millis;
  lanOps.epoch = [] { return ph_auth_credential_epoch(&localAuth); };
  lanOps.random = [](uint8_t nonce[16]) { return RAND_bytes(nonce, 16) == 1; };
  lanOps.load = [](const char *peer, char key[33], unsigned *scopes) {
    return ph_auth_remote_get(&localAuth, peer, key, scopes);
  };
  lanOps.save = [](const char *peer, const char *key, unsigned scopes, uint32_t epoch) {
    return ph_auth_remote_store(&localAuth, peer, key, scopes, epoch);
  };
  lanOps.send = [](const char *ip, const void *bytes, size_t length) {
    assert(!std::strcmp(ip, "127.0.0.1")); return sendPacket(bytes, length);
  };
  lanOps.business_boot = [] { return hubLanBoot; };
  config["hub"]["username"] = "owner";
  auto devices = config["hub"].createNestedArray("devices");
  const auto prefix = hubService.prefix(), base = prefix + "s30-v1b/" + peerId;
  const std::map<std::string, std::string> txt{{"id", peerId}, {"model", "s30-v1b"},
      {"variant", "full"}, {"firmware", "1.0.0-host-dirty"},
      {"lan_wire", "4"}, {"codec", "msgpack"}};
  assert(hubLanDiscovery.observe(txt, "127.0.0.1", uint16_t(port), millis()));
  const auto route = base + "/command", initial = request("status.get", "initial");
  auto blocked = [&](const char *code) {
    const auto count = commands;
    const char *actual = hubLanStart(route, initial, false);
    assert(actual && !std::strcmp(actual, code) && commands == count);
  };
  blocked("auth_required"); // mDNS alone cannot register or authorize a device.
  auto saved = devices.createNestedObject(); saved["mac"] = peerId;
  saved["model"] = "s30-v1b"; saved["version"] = "v2";
  saved["status"] = "active"; saved["username"] = "owner";
  blocked("auth_required"); // Registration alone cannot mint a session.
  const char *code = nullptr;
  assert(lc_start(&lanClient, &lanOps, peerId.c_str(), "127.0.0.1", "wrong", PH_AUTH_SCOPE_CONTROL, &code));
  until([] { return lanClient.jobs[0].phase == LC_FAILED; });
  assert(!hubLanPaired(peerId));
  assert(lc_start(&lanClient, &lanOps, peerId.c_str(), "127.0.0.1", argv[3], PH_AUTH_SCOPE_CONTROL, &code));
  until([] { return lanClient.jobs[0].phase == LC_READY; });
  assert(hubLanPaired(peerId) && persisted.remote_count == 1 && pairs == 2 && resumes == 1);
  blocked("auth_required"); // Even a valid token requires a confirmed route.
  auto confirmed = config.createNestedArray("lan_peers").createNestedObject();
  confirmed["peer"] = peerId; confirmed["ip"] = "127.0.0.1";
  saved["model"] = "tc1"; blocked("auth_required"); saved["model"] = "s30-v1b";
  saved["status"] = "inactive"; blocked("auth_required"); saved["status"] = "active";
  confirmed["ip"] = "127.0.0.2"; blocked("auth_required"); confirmed["ip"] = "127.0.0.1";
  ready = false; blocked("busy"); ready = true;
  heapReady = false; blocked("busy"); heapReady = true;

#include "hub-s30-authorize.inc"
  hubLanClient.send = [](const std::string &ip, uint16_t udpPort, const void *bytes, size_t n) {
    assert(ip == "127.0.0.1" && udpPort == ntohs(destination.sin_port));
    return sendPacket(bytes, n);
  };
  iotcloud::HubDeviceBus bus;
  iotcloud::HubHome home(bus);
  assert(home.configure(config["hub"]["devices"], prefix, deviceId));
  unsigned serial = 0;
  home.randomHex = [&](size_t n) { auto s = std::to_string(++serial); return std::string(n * 2 - s.size(), '0') + s; };
  bus.dispatch = [&](const std::string &topic, const std::string &body, bool retain, iotcloud::HubDeviceBus::Origin origin) {
    if (topic == route) return !hubLanStart(topic, body, retain);
    assert(origin == iotcloud::HubDeviceBus::Origin::Local && !retain);
    home.tick(millis()); home.observe(topic, body); return true;
  };
  hubLanClient.complete = [&](const std::string &topic, const void *bytes, size_t n) {
    const bool status = hubLanRequestStatus;
    const std::string expected = hubLanRequestId;
    lastReply.assign(static_cast<const char *>(bytes), n); ++completed;
    assert(iotcloud::routeHubLanResult(bus, topic, bytes, n, status, expected));
  };
  home.devices[1].query = "initial";
  assert(!hubLanStart(route, initial, false));
  until([] { return !hubLanClient.busy(); });
  assert(home.devices[1].controls && !home.devices[1].outlets[0] && businessCode() == "ok");
  home.tick(millis());
  auto count = commands;
  for (unsigned channel : {1u, 2u, 5u, 6u, 999u})
    assert(!home.begin(1, iotcloud::HubHome::Outlet, channel, 1));
  assert(count == commands);
  assert(home.begin(1, iotcloud::HubHome::Outlet, 0, 1));
  until([&] { return home.finished(); });
  assert(home.finish() && home.devices[1].outlets[0] && !hubLanClient.busy());
  for (unsigned i = 1; i < 6; ++i) assert(!home.devices[1].outlets[i]);
  const auto invalid = request("outlet.set", "invalid-index", 1);
  assert(!hubLanStart(route, invalid, false));
  until([] { return !hubLanClient.busy(); });
  assert(businessCode() == "invalid_params" && home.devices[1].outlets[0]);
  // The persisted direction-specific key resumes a new hub boot; no fake token.
  const auto oldPairCount = pairs, oldResumeCount = resumes;
  lanClient = lc_state{}; ++hubLanBoot;
  uint8_t token[16];
  assert(!lc_token(&lanClient, &lanOps, peerId.c_str(), "127.0.0.1", PH_AUTH_SCOPE_CONTROL, token));
  until([] { return lanClient.jobs[0].phase == LC_READY; });
  assert(pairs == oldPairCount && resumes == oldResumeCount + 1);
  home.devices[1].query = "resumed";
  assert(!hubLanStart(route, request("status.get", "resumed"), false));
  until([] { return !hubLanClient.busy(); });
  assert(home.devices[1].outlets[0] && businessCode() == "ok");
  assert(home.begin(1, iotcloud::HubHome::Outlet, 0, 0));
  until([&] { return home.finished(); });
  assert(home.finish() && !home.devices[1].outlets[0]);
  if (!pulls) {
    assert(!hubLanStart(route, request("diagnostics.get", "chunked"), false));
    until([] { return !hubLanClient.busy(); });
    assert(businessCode() == "ok");
  }
  assert(pulls > 0 && acks > 0 && completed >= 7);
  std::printf("PASS real S30 endpoint + production hub gates/lc_core/v4 sender/HAP model: wrong password denied, pair=%u resume=%u command=%u pull=%u ACK=%u; ephemeral source port, confirmed IP/registration required, bool state len1, index1+ rejection, on/off measured after receipt, reboot resume\n",
              pairs, resumes, commands, pulls, acks);
  hubLanClient.reset(); close(clientfd);
}
