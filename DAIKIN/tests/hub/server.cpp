#include "hub_transport.h"
#include <chrono>
#include <csignal>
#include <cstdio>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <thread>
static volatile sig_atomic_t running = 1;
static void stop(int) { running = 0; }
int main(int argc, char **argv) {
  if (argc != 3)
    return 2;
  iotcloud::HubTransport transport;
  transport.websocketAccept = [](const std::string &key) {
    auto input = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    unsigned char hash[20], encoded[32];
    SHA1((const unsigned char *)input.data(), input.size(), hash);
    EVP_EncodeBlock(encoded, hash, sizeof(hash));
    return std::string((char *)encoded);
  };
  transport.broker.authenticate = [](const std::string &u,
                                     const std::string &p) {
    return u == "test" && p == "test-password";
  };
  transport.broker.authorize = [](const std::string &topic, bool) {
    return topic.find("phicomm/v2/") != 0 ||
           topic.find("phicomm/v2/u_74657374/") == 0;
  };
  if (!transport.start(atoi(argv[1]), atoi(argv[2])))
    return 3;
  signal(SIGTERM, stop);
  signal(SIGINT, stop);
  puts("ready");
  fflush(stdout);
  auto began = std::chrono::steady_clock::now();
  while (running) {
    transport.poll(std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - began)
                       .count());
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}
