// Host-only HTTP adapter is in frontend/tests/hub-console.mjs. The account,
// broker and relay implementations below are exactly those used by the ESP32.
#include "hub_relay.h"
#include "hub_transport.h"
#include <chrono>
#include <csignal>
#include <cstdio>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <thread>
static volatile sig_atomic_t running = 1;
static void stop(int) { running = 0; }
static std::string hex(const uint8_t *p, size_t n) {
  std::string s;
  for (size_t i = 0; i < n; i++) {
    s += "0123456789abcdef"[p[i] >> 4];
    s += "0123456789abcdef"[p[i] & 15];
  }
  return s;
}
static void emit(const DynamicJsonDocument &doc) {
  std::string s;
  serializeJson(doc, s);
  puts(s.c_str());
  fflush(stdout);
}
int main(int argc, char **argv) {
  if (argc != 3)
    return 2;
  DynamicJsonDocument config(16384);
  config["auth"]["password"] = "fixture-device-password";
  iotcloud::HubTransport transport;
  iotcloud::HubService service(config);
  iotcloud::HubRelay relay(config, service, transport.broker);
  auto random = [](size_t n) {
    uint8_t p[32];
    if (n > sizeof(p) || RAND_bytes(p, n) != 1)
      return std::string();
    return hex(p, n);
  };
  service.randomHex = random;
  relay.randomHex = random;
  service.derive = [](const std::string &password, const std::string &salt) {
    uint8_t p[32];
    if (PKCS5_PBKDF2_HMAC(password.data(), password.size(),
                          (const uint8_t *)salt.data(), salt.size(), 10000,
                          EVP_sha256(), sizeof(p), p) != 1)
      return std::string();
    return hex(p, sizeof(p));
  };
  service.store = [&](DynamicJsonDocument &next) {
    config.set(next);
    return true;
  };
  service.changed = [&](bool credentials) {
    if (credentials)
      transport.broker.reset();
    relay.reset();
  };
  service.bemfaStatus = [&]() { return relay.status; };
  uint32_t clock = 0;
  transport.broker.authenticate = [&](const std::string &u,
                                      const std::string &p) {
    return service.authenticate(u, p, clock);
  };
  transport.broker.authorize = [&](const std::string &t, bool w) {
    return service.allowed(t, w);
  };
  transport.broker.event = [&](const std::string &t, const std::string &b,
                               bool r, iotcloud::HubBroker::Origin o) {
    relay.local(t, b, r, o);
  };
  auto forwarded = [](const char *event, const std::string &topic,
                      const std::string &body, bool retain) {
    DynamicJsonDocument doc(16384);
    doc["event"] = event;
    doc["topic"] = topic;
    doc["payload"] = body;
    doc["retain"] = retain;
    emit(doc);
    return true;
  };
  relay.upstream = [&](const std::string &t, const std::string &b, bool r) {
    return forwarded("upstream", t, b, r);
  };
  relay.bemfa = [&](const std::string &t, const std::string &b, bool r) {
    return forwarded("bemfa", t, b, r);
  };
  transport.websocketAccept = [](const std::string &key) {
    auto input = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    uint8_t p[20], encoded[32];
    SHA1((const uint8_t *)input.data(), input.size(), p);
    EVP_EncodeBlock(encoded, p, sizeof(p));
    return std::string((char *)encoded);
  };
  if (!transport.start(atoi(argv[1]), atoi(argv[2])))
    return 3;
  signal(SIGTERM, stop);
  signal(SIGINT, stop);
  fcntl(STDIN_FILENO, F_SETFL, fcntl(STDIN_FILENO, F_GETFL, 0) | O_NONBLOCK);
  puts("{\"ready\":true}");
  fflush(stdout);
  std::string input;
  auto began = std::chrono::steady_clock::now();
  while (running) {
    clock = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - began)
                .count();
    relay.tick(clock);
    transport.poll(clock);
    char buffer[1024];
    int n = read(STDIN_FILENO, buffer, sizeof(buffer));
    if (n > 0)
      input.append(buffer, n);
    size_t end;
    while ((end = input.find('\n')) != std::string::npos) {
      auto line = input.substr(0, end);
      input.erase(0, end + 1);
      DynamicJsonDocument doc(16384), out(24576);
      if (deserializeJson(doc, line))
        return 4;
      out["id"] = doc["id"];
      if (doc["op"] == "incoming") {
        if (doc["source"] == 1)
          relay.incomingBemfa(doc["topic"] | "", doc["payload"] | "",
                              doc["retain"] | false);
        else
          relay.incoming(doc["topic"] | "", doc["payload"] | "",
                         doc["retain"] | false);
        out["status"] = 200;
        out.createNestedObject("body");
      } else {
        iotcloud::HubService::Request request(
            doc["method"] | "GET", doc["path"] | "", doc["body"] | "",
            doc["token"] | "", doc["host"] | "127.0.0.1", doc["setup"] | false);
        auto reply = service.handle(request, clock);
        out["status"] = reply.status;
        out["token"] = reply.token;
        DynamicJsonDocument body(16384);
        if (deserializeJson(body, reply.body))
          return 5;
        out["body"] = body;
      }
      emit(out);
    }
    if (input.size() > 24576)
      return 6;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}
