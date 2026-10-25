/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

// Actual allocations made during production poll(), not a substitute buffer.
// The fake byte heap can fragment after a socket has already been admitted.
static size_t freeByte = 45980, largestByte = 11764;
static bool measure = false, failHttp = false;
static unsigned oversized = 0, httpFailures = 0;
static size_t httpLargest = 0;
struct alignas(std::max_align_t) Block { size_t size; bool measured; };
static void *allocateBlock(size_t n, bool nothrow) {
  if (measure && (n > largestByte || n > freeByte)) {
    ++oversized;
    if (nothrow) return nullptr;
    throw std::bad_alloc();
  }
  auto p = static_cast<Block *>(std::malloc(sizeof(Block) + (n ? n : 1)));
  if (!p) {
    if (nothrow) return nullptr;
    throw std::bad_alloc();
  }
  p->size = n; p->measured = measure;
  if (measure) freeByte -= n;
  return p + 1;
}
void *operator new(size_t n) { return allocateBlock(n, false); }
void *operator new[](size_t n) { return allocateBlock(n, false); }
void *operator new[](size_t n, const std::nothrow_t &) noexcept {
  if (measure) httpLargest = std::max(httpLargest, n);
  if (failHttp) { failHttp = false; ++httpFailures; return nullptr; }
  return allocateBlock(n, true);
}
void operator delete(void *p) noexcept {
  if (!p) return;
  auto block = static_cast<Block *>(p) - 1;
  if (block->measured) freeByte += block->size;
  std::free(block);
}
void operator delete[](void *p) noexcept { ::operator delete(p); }

#include "hub_transport.h"
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <limits>

// Force a real partial transport send without changing the production poll.
static size_t sendLimit = 0;
extern "C" ssize_t __real_send(int, const void *, size_t, int);
extern "C" ssize_t __wrap_send(int fd, const void *p, size_t n, int flags) {
  if (sendLimit && (flags & MSG_NOSIGNAL)) n = std::min(n, sendLimit);
  return __real_send(fd, p, n, flags);
}

static const unsigned MALLOC_CAP_INTERNAL = 1, MALLOC_CAP_8BIT = 2;
static size_t heap_caps_get_free_size(unsigned caps) {
  assert(caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)); return freeByte;
}
static size_t heap_caps_get_largest_free_block(unsigned caps) {
  assert(caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)); return largestByte;
}
#define MBEDTLS_VERSION_MAJOR 3
static int mbedtls_sha1(const uint8_t *p, size_t n, uint8_t *out) {
  return SHA1(p, n, out) ? 0 : -1;
}
static int mbedtls_base64_encode(uint8_t *out, size_t capacity, size_t *length,
                                 const uint8_t *p, size_t n) {
  if (capacity < 4 * ((n + 2) / 3) + 1) return -1;
  const int encoded = EVP_EncodeBlock(out, p, n);
  if (encoded < 0) return -1;
  *length = size_t(encoded); return 0;
}
#include "allocation-ready.inc"

using Bytes = std::vector<uint8_t>;
static void text(Bytes &out, const std::string &value) {
  out.push_back(value.size() >> 8); out.push_back(value.size() & 255);
  out.insert(out.end(), value.begin(), value.end());
}
static Bytes packet(uint8_t type, const Bytes &body) {
  Bytes out{type}; size_t n = body.size();
  do { out.push_back(n % 128 | (n >= 128 ? 128 : 0)); n /= 128; } while (n);
  out.insert(out.end(), body.begin(), body.end()); return out;
}
static Bytes connectPacket(unsigned id = 0) {
  Bytes body; text(body, "MQTT"); body.insert(body.end(), {4, 0xc2, 0, 30});
  text(body, "peer" + std::to_string(id)); text(body, "test"); text(body, "secret");
  return packet(0x10, body);
}
static Bytes frame(uint8_t opcode, const Bytes &body) {
  Bytes out{uint8_t(0x80 | opcode)};
  if (body.size() < 126) out.push_back(uint8_t(0x80 | body.size()));
  else out.insert(out.end(), {0xfe, uint8_t(body.size() >> 8), uint8_t(body.size())});
  const uint8_t mask[] = {1, 2, 3, 4};
  out.insert(out.end(), mask, mask + 4);
  for (size_t i = 0; i < body.size(); ++i) out.push_back(body[i] ^ mask[i % 4]);
  return out;
}
struct Fixture {
  iotcloud::HubTransport transport;
  uint16_t tcp, ws; uint32_t now = 0;
  explicit Fixture(uint16_t tcp, uint16_t ws) : tcp(tcp), ws(ws) {
#include "admission-assignments.inc"
    transport.broker.authenticate = [](const std::string &u, const std::string &p) {
      return u == "test" && p == "secret";
    };
    transport.broker.authorize = [](const std::string &, bool) { return true; };
    assert(transport.start(tcp, ws));
  }
  void poll() {
    measure = true; transport.poll(++now); measure = false;
    assert(!oversized);
  }
  int client(bool websocket, bool accepted = true) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0); assert(fd >= 0);
    timeval timeout{1, 0}; assert(!setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)));
    sockaddr_in address{}; address.sin_family = AF_INET;
    address.sin_port = htons(websocket ? ws : tcp);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(!::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)));
    poll(); if (!accepted) closed(fd); return fd;
  }
  static void send(int fd, const Bytes &bytes) {
    assert(::send(fd, bytes.data(), bytes.size(), 0) == ssize_t(bytes.size()));
  }
  static Bytes read(int fd, size_t n) {
    Bytes result(n); size_t at = 0;
    while (at < n) { const auto got = ::recv(fd, result.data() + at, n - at, 0); assert(got > 0); at += got; }
    return result;
  }
  static void closed(int fd) {
    uint8_t b; const auto n = ::recv(fd, &b, 1, 0);
    assert(n == 0 || (n < 0 && errno == ECONNRESET));
  }
  void upgrade(int fd, const std::string &extra = "") {
    const std::string request =
        "GET /mqtt HTTP/1.1\r\nHost: localhost\r\nuPgRaDe: WebSocket\r\n"
        "Connection: keep-alive, UpGrade\r\nSec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Protocol: other, mQtT\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n" + extra + "\r\n";
    send(fd, Bytes(request.begin(), request.end())); poll(); poll();
    std::string response;
    while (response.find("\r\n\r\n") == std::string::npos) {
      auto b = read(fd, 1); response.push_back(b[0]);
    }
    assert(response.find("101 Switching Protocols") != std::string::npos);
    assert(response.find("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != std::string::npos);
  }
  void connect(int fd, bool websocket, unsigned id = 0) {
    auto input = connectPacket(id); if (websocket) input = frame(2, input);
    send(fd, input); poll();
    const Bytes ack{0x20, 2, 0, 0};
    if (websocket) assert(read(fd, 2) == Bytes({0x82, 4}));
    assert(read(fd, 4) == ack);
  }
};
static void small(uint16_t tcp, uint16_t ws, bool websocket) {
  Fixture f(tcp, ws);
  // This was rejected by the actual previous admission (>12288 largest).
  assert(freeByte == 45980 && largestByte == 11764);
  int fd = f.client(websocket);
  if (websocket) f.upgrade(fd);
  f.connect(fd, websocket); assert(f.transport.broker.count() == 1);
  assert(!f.transport.broker.refused && !f.transport.broker.dropped);
  assert(websocket ? httpLargest <= 513 && httpLargest >= 257 : !httpLargest);
  // Fixed frame/control storage permits a full control payload even after the
  // original HTTP allocation has been released and the heap fragments again.
  if (websocket) {
    largestByte = 1025; freeByte = 45980;
    Bytes payload(125, 'p'); Fixture::send(fd, frame(9, payload)); f.poll();
    assert(Fixture::read(fd, 2) == Bytes({0x8a, 125}));
    assert(Fixture::read(fd, payload.size()) == payload);
  }
  ::close(fd); f.poll(); assert(!f.transport.broker.count());
}
static void fragmented(uint16_t tcp, uint16_t ws, bool websocket) {
  largestByte = 1025;
  Fixture f(tcp, ws); int fd = f.client(websocket);
  if (websocket) f.upgrade(fd);
  f.connect(fd, websocket); assert(f.transport.broker.count() == 1);
  ::close(fd); f.poll();
}
static void boundaries(uint16_t tcp, uint16_t ws) {
  Fixture f(tcp, ws);
  const size_t initial = iotcloud::HubTransport::InitialAllocation;
  freeByte = initial + 16384; largestByte = initial + 1;
  int fd = f.client(false, false); ::close(fd);
  ++freeByte; largestByte = initial;
  fd = f.client(true, false); ::close(fd);
  ++largestByte;
  fd = f.client(false); assert(f.transport.broker.active(0));
  ::close(fd); f.poll(); assert(!f.transport.broker.active(0));
  assert(!hubAllocationReady(std::numeric_limits<size_t>::max()));
}
static void existing(uint16_t tcp, uint16_t ws) {
  Fixture f(tcp, ws); int first = f.client(false); f.connect(first, false);
  freeByte = 17000; largestByte = 2000;
  int next = f.client(true, false); ::close(next);
  Fixture::send(first, Bytes{0xc0, 0}); f.poll();
  assert(Fixture::read(first, 2) == Bytes({0xd0, 0}));
  assert(f.transport.broker.count() == 1 && !f.transport.broker.refused);
  ::close(first); f.poll();
}
static void growth(uint16_t tcp, uint16_t ws, bool websocket) {
  Fixture f(tcp, ws); int fd = f.client(websocket);
  if (websocket) f.upgrade(fd);
  f.connect(fd, websocket);
  largestByte = 1500;
  Bytes body; text(body, "topic"); body.insert(body.end(), 4096, 'p');
  auto message = packet(0x30, body); if (websocket) message = frame(2, message);
  Fixture::send(fd, message); for (unsigned i = 0; i < 4; ++i) f.poll();
  Fixture::closed(fd); ::close(fd);
  assert(!f.transport.broker.count() && f.transport.broker.refused == 1);
}
static void httpGrowth(uint16_t tcp, uint16_t ws, bool actualFailure) {
  Fixture f(tcp, ws); int fd = f.client(true);
  const size_t before = freeByte;
  const std::string first = "GET /mqtt HTTP/1.1\r\nX: " + std::string(400, 'a');
  Fixture::send(fd, Bytes(first.begin(), first.end())); f.poll();
  assert(httpLargest == 513 && freeByte < before);
  if (actualFailure) failHttp = true; else largestByte = 1025;
  const Bytes more(700, 'b'); Fixture::send(fd, more); f.poll(); f.poll();
  Fixture::closed(fd); ::close(fd);
  assert(!f.transport.broker.active(0) && freeByte == before);
  assert(actualFailure ? httpFailures == 1 : !httpFailures);
}
static void longHeaders(uint16_t tcp, uint16_t ws) {
  largestByte = 2548;
  Fixture f(tcp, ws); int fd = f.client(true);
  f.upgrade(fd, std::string(900, 'x') + ": " + std::string(700, 'y') + "\r\n");
  assert(httpLargest == 2049);
  f.connect(fd, true); ::close(fd); f.poll();
}
static void limit(uint16_t tcp, uint16_t ws) {
  Fixture f(tcp, ws); int fd = f.client(true);
  Fixture::send(fd, Bytes(iotcloud::HubTransport::HttpLimit + 1, 'x'));
  f.poll(); f.poll(); Fixture::closed(fd); ::close(fd);
  assert(httpLargest == iotcloud::HubTransport::HttpLimit + 1);
  assert(!f.transport.broker.active(0));
}
static void partialTx(uint16_t tcp, uint16_t ws) {
  Fixture f(tcp, ws); int fd = f.client(false); f.connect(fd, false);
  largestByte = 25000;
  const Bytes bytes(20000, 't');
  measure = true;
  assert(f.transport.broker.send(0, bytes.data(), bytes.size()));
  measure = false;
  const size_t queuedFree = freeByte;
  largestByte = 128; sendLimit = 1000;
  f.poll(); assert(Fixture::read(fd, 1000) == Bytes(1000, 't'));
  assert(freeByte == queuedFree); // No replacement while partially sent.
  sendLimit = 0;
  f.poll(); assert(Fixture::read(fd, 19000) == Bytes(19000, 't'));
  assert(freeByte == queuedFree + bytes.size()); // Empty buffer released.
  ::close(fd); f.poll();
}
static void clients(uint16_t tcp, uint16_t ws) {
  Fixture f(tcp, ws); int sockets[iotcloud::HubBroker::Clients];
  for (unsigned i = 0; i < iotcloud::HubBroker::Clients; ++i) {
    const bool websocket = i % 2;
    sockets[i] = f.client(websocket);
    if (websocket) f.upgrade(sockets[i]);
    f.connect(sockets[i], websocket, i);
    assert(f.transport.broker.count() == i + 1);
  }
  for (bool websocket : {false, true}) {
    int excess = f.client(websocket, false); ::close(excess);
  }
  Fixture::send(sockets[0], Bytes{0xc0, 0}); f.poll();
  assert(Fixture::read(sockets[0], 2) == Bytes({0xd0, 0}));
  assert(f.transport.broker.count() == iotcloud::HubBroker::Clients);
  for (int fd : sockets) ::close(fd);
  f.poll(); assert(!f.transport.broker.count());
}
int main(int argc, char **argv) {
  assert(argc == 4); const std::string name = argv[1];
  const uint16_t tcp = atoi(argv[2]), ws = atoi(argv[3]);
  if (name == "tcp-small" || name == "ws-small") small(tcp, ws, name == "ws-small");
  else if (name == "tcp-fragmented" || name == "ws-fragmented") fragmented(tcp, ws, name == "ws-fragmented");
  else if (name == "boundaries") boundaries(tcp, ws);
  else if (name == "existing") existing(tcp, ws);
  else if (name == "tcp-growth" || name == "ws-growth") growth(tcp, ws, name == "ws-growth");
  else if (name == "http-growth" || name == "http-failure") httpGrowth(tcp, ws, name == "http-failure");
  else if (name == "long-headers") longHeaders(tcp, ws);
  else if (name == "partial-tx") partialTx(tcp, ws);
  else if (name == "clients") clients(tcp, ws);
  else { assert(name == "http-limit"); limit(tcp, ws); }
  assert(!oversized); std::printf("PASS %s\n", name.c_str());
}
