/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "hub_broker.h"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <new>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

namespace iotcloud {
// The application loop owns sockets, protocol state and all callbacks.
class HubTransport {
public:
  static constexpr size_t TxBudget = 32768, HttpLimit = 2048, ReadBlock = 512,
                          InitialAllocation = ReadBlock + 512;
  HubBroker broker;
  std::function<std::string(const std::string &)> websocketAccept;
  std::function<bool()> admission;
  bool start(uint16_t tcp = 1883, uint16_t websocket = 9001) {
    stop();
    listeners[0] = listenOn(tcp);
    listeners[1] = listenOn(websocket);
    if (listeners[0] < 0 || listeners[1] < 0) {
      stop();
      return false;
    }
    return true;
  }
  void stop() {
    broker.reset();
    for (unsigned i = 0; i < HubBroker::Clients; i++)
      finish(i);
    for (auto &fd : listeners) {
      if (fd >= 0)
        ::close(fd);
      fd = -1;
    }
  }
  HubTransport() {
    broker.writable = [this](unsigned i, size_t n) {
      return peers[i].fd >= 0 && !peers[i].closing &&
             queued() + n + 4 <= TxBudget;
    };
    broker.send = [this](unsigned i, const uint8_t *p, size_t n) {
      return enqueue(i, p, n);
    };
    broker.sendPublish = [this](unsigned i, const uint8_t *header, size_t n,
                               const std::string &topic,
                               const std::string &body) {
      auto &p = peers[i];
      const size_t mqttLength = n + topic.size() + body.size();
      const size_t frameLength = p.ws ? (mqttLength >= 126 ? 4 : 2) : 0;
      const size_t total = mqttLength + frameLength;
      if ((p.ws && !p.upgraded) || !prepare(i, total))
        return false;
      if (p.ws) {
        p.tx.push_back(0x82);
        p.tx.push_back(mqttLength >= 126 ? 126 : mqttLength);
        if (mqttLength >= 126) {
          p.tx.push_back(mqttLength >> 8);
          p.tx.push_back(mqttLength & 255);
        }
      }
      p.tx.insert(p.tx.end(), header, header + n);
      p.tx.insert(p.tx.end(), topic.begin(), topic.end());
      p.tx.insert(p.tx.end(), body.begin(), body.end());
      return true;
    };
    broker.close = [this](unsigned i) {
      if (peers[i].fd >= 0) {
        peers[i].closing = true;
        peers[i].closedAt = clock;
      }
    };
  }
  ~HubTransport() { stop(); }
  HubTransport(const HubTransport &) = delete;
  HubTransport &operator=(const HubTransport &) = delete;
  bool running() const { return listeners[0] >= 0 && listeners[1] >= 0; }
  void poll(uint32_t now) {
    clock = now;
    for (unsigned type = 0; type < 2; type++)
      if (listeners[type] >= 0) {
        for (unsigned accepted = 0; accepted < 2; accepted++) {
          int fd = ::accept(listeners[type], nullptr, nullptr);
          if (fd < 0)
            break;
          unsigned slot = 0;
          while (slot < peers.size() && peers[slot].fd >= 0)
            slot++;
          if (slot == peers.size() || (admission && !admission()) ||
              !nonblocking(fd)) {
            ::close(fd);
            continue;
          }
          int one = 1;
          setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
          peers[slot] = Peer();
          peers[slot].fd = fd;
          peers[slot].ws = type;
          peers[slot].at = now;
          if (!broker.open(slot, now))
            finish(slot);
        }
      }
    uint8_t buffer[ReadBlock];
    for (unsigned i = 0; i < peers.size(); i++) {
      auto &p = peers[i];
      if (p.fd < 0)
        continue;
      if (!p.closing)
        for (unsigned reads = 0; reads < 4; reads++) {
          int n = ::recv(p.fd, buffer, sizeof(buffer), 0);
          if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
          if (n <= 0) {
            drop(i);
            break;
          }
          bool ok = p.ws ? websocket(i, buffer, n)
                         : broker.receive(i, buffer, n, now);
          if (!ok) {
            if (broker.active(i))
              broker.disconnect(i);
            break;
          }
        }
      if (p.fd < 0)
        continue;
      if (!p.tx.empty()) {
        int flags = 0;
#ifdef MSG_NOSIGNAL
        flags = MSG_NOSIGNAL;
#endif
        int n = ::send(p.fd, p.tx.data(), p.tx.size(), flags);
        if (n > 0) {
          p.tx.erase(p.tx.begin(), p.tx.begin() + n);
          // Releasing an empty buffer cannot fail. Shrinking a partially sent
          // vector would allocate an unchecked replacement under fragmentation.
          if (p.tx.empty() && p.tx.capacity() > 1024)
            std::vector<uint8_t>().swap(p.tx);
          p.at = now;
        } else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
          drop(i);
          continue;
        }
      }
      if ((p.closing && (p.tx.empty() || uint32_t(now - p.closedAt) > 250)) ||
          (!p.tx.empty() && uint32_t(now - p.at) > 5000))
        drop(i);
    }
    broker.tick(now);
  }

private:
  struct HttpBuffer {
    std::unique_ptr<char[]> data;
    size_t size = 0, capacity = 0;
    bool append(uint8_t c, const std::function<bool(size_t)> &allocate) {
      if (!c || size == HttpLimit)
        return false;
      if (size == capacity) {
        const size_t next = std::min(size_t(HttpLimit), std::max(size_t(32), capacity * 2));
        // Keep the previous block live until the complete replacement exists.
        if (allocate && !allocate(next + 1))
          return false;
        std::unique_ptr<char[]> replacement(new (std::nothrow) char[next + 1]);
        if (!replacement)
          return false;
        if (size)
          memcpy(replacement.get(), data.get(), size);
        data.swap(replacement);
        capacity = next;
      }
      data[size++] = c;
      data[size] = 0;
      return true;
    }
  };
  struct Peer {
    int fd = -1;
    bool ws = false, upgraded = false, closing = false, fragment = false;
    uint32_t at = 0, closedAt = 0;
    std::vector<uint8_t> tx;
    HttpBuffer http;
    // WebSocket headers/control payloads have fixed protocol bounds. These
    // slots exist before accept(), so a peer cannot trigger hidden growth.
    uint8_t header[14] = {}, control[125] = {};
    size_t headerSize = 0, controlSize = 0;
    uint64_t remaining = 0, offset = 0;
    uint8_t mask[4] = {}, opcode = 0;
    bool fin = false;
  };
  std::array<Peer, HubBroker::Clients> peers;
  int listeners[2] = {-1, -1};
  uint32_t clock = 0;
  static bool nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
  }
  static int listenOn(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
      return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (!nonblocking(fd) || ::bind(fd, (sockaddr *)&addr, sizeof(addr)) < 0 ||
        ::listen(fd, 4) < 0) {
      ::close(fd);
      return -1;
    }
    return fd;
  }
  void finish(unsigned i) {
    if (peers[i].fd >= 0)
      ::close(peers[i].fd);
    peers[i] = Peer();
  }
  void drop(unsigned i) {
    broker.disconnect(i);
    finish(i);
  }
  size_t queued() const {
    size_t n = 0;
    for (const auto &p : peers)
      n += p.tx.size();
    return n;
  }
  bool prepare(unsigned i, size_t n) {
    auto &p = peers[i];
    if (p.fd < 0 || p.closing || queued() + n > TxBudget)
      return false;
    if (!n)
      return true;
    const size_t size = p.tx.size() + n;
    // reserve() needs the whole replacement block while the previous buffer
    // remains live. Checking only the appended bytes can admit an OOM.
    if (size > p.tx.capacity() && broker.allocate && !broker.allocate(size))
      return false;
    if (p.tx.empty())
      p.at = clock;
    p.tx.reserve(size);
    return true;
  }
  bool raw(unsigned i, const uint8_t *data, size_t n) {
    if (!prepare(i, n))
      return false;
    auto &p = peers[i];
    p.tx.insert(p.tx.end(), data, data + n);
    return true;
  }
  bool frame(unsigned i, uint8_t opcode, const uint8_t *data, size_t n) {
    uint8_t head[4] = {uint8_t(0x80 | opcode), uint8_t(n), 0, 0};
    size_t length = 2;
    if (n >= 126) {
      head[1] = 126;
      head[2] = n >> 8;
      head[3] = n & 255;
      length = 4;
    }
    if (queued() + n + length > TxBudget)
      return false;
    return raw(i, head, length) && raw(i, data, n);
  }
  bool enqueue(unsigned i, const uint8_t *data, size_t n) {
    return peers[i].ws ? peers[i].upgraded && frame(i, 2, data, n)
                       : raw(i, data, n);
  }
  struct Text {
    const char *data = nullptr;
    size_t size = 0;
    Text() = default;
    Text(const char *data, size_t size) : data(data), size(size) {}
    bool equals(const char *lower) const {
      if (size != strlen(lower))
        return false;
      for (size_t i = 0; i < size; ++i) {
        auto c = static_cast<unsigned char>(data[i]);
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (c != static_cast<unsigned char>(lower[i])) return false;
      }
      return true;
    }
    bool token(const char *wanted) const {
      for (size_t at = 0; at < size;) {
        size_t end = at;
        while (end < size && data[end] != ',') ++end;
        size_t first = at, last = end;
        while (first < last && data[first] == ' ') ++first;
        while (last > first && data[last - 1] == ' ') --last;
        if (Text(data + first, last - first).equals(wanted)) return true;
        at = end + 1;
      }
      return false;
    }
  };
  static Text field(const HttpBuffer &http, const char *name) {
    const char *at = strstr(http.data.get(), "\r\n");
    while (at) {
      at += 2;
      const char *end = strstr(at, "\r\n");
      if (!end || end == at) break;
      const char *colon = static_cast<const char *>(memchr(at, ':', end - at));
      if (colon && Text(at, colon - at).equals(name)) {
        const char *first = colon + 1;
        while (first < end && (*first == ' ' || *first == '\t')) ++first;
        while (end > first && (end[-1] == ' ' || end[-1] == '\t')) --end;
        return Text(first, end - first);
      }
      at = end;
    }
    return {};
  }
  bool upgrade(unsigned i) {
    auto &p = peers[i];
    auto key = field(p.http, "sec-websocket-key");
    if (p.http.size < 20 || memcmp(p.http.data.get(), "GET /mqtt HTTP/1.1\r\n", 20) ||
        !field(p.http, "sec-websocket-version").equals("13") ||
        !field(p.http, "connection").token("upgrade") ||
        !field(p.http, "upgrade").equals("websocket") ||
        !field(p.http, "sec-websocket-protocol").token("mqtt") ||
        key.size != 24 || !websocketAccept ||
        (broker.allocate && !broker.allocate(512)))
      return false;
    // Only bounded key/SHA/base64 temporaries remain after parsing spans. The
    // complete response lives on the stack, then uses the normal TX guard.
    std::string answer = websocketAccept(std::string(key.data, key.size));
    if (answer.size() != 28)
      return false;
    char response[192];
    const int length = snprintf(response, sizeof(response),
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: "
        "Upgrade\r\nSec-WebSocket-Protocol: mqtt\r\nSec-WebSocket-Accept: %s\r\n\r\n",
        answer.c_str());
    if (length < 0 || size_t(length) >= sizeof(response) ||
        !raw(i, (const uint8_t *)response, size_t(length)))
      return false;
    p.upgraded = true;
    p.http = HttpBuffer();
    return true;
  }
  bool websocket(unsigned i, uint8_t *data, size_t n) {
    auto &p = peers[i];
    size_t at = 0;
    while (at < n && !p.closing) {
      if (!p.upgraded) {
        if (!p.http.append(data[at++], broker.allocate))
          return false;
        if (p.http.size >= 4 &&
            !memcmp(p.http.data.get() + p.http.size - 4, "\r\n\r\n", 4) &&
            !upgrade(i))
          return false;
        continue;
      }
      if (!p.remaining) {
        if (p.headerSize == sizeof(p.header))
          return false;
        p.header[p.headerSize++] = data[at++];
        if (p.headerSize < 2)
          continue;
        unsigned length = p.header[1] & 127;
        size_t need = 2 + (length == 126 ? 2 : length == 127 ? 8 : 0) + 4;
        if (p.headerSize < need)
          continue;
        p.opcode = p.header[0] & 15;
        p.fin = p.header[0] & 128;
        if ((p.header[0] & 0x70) || !(p.header[1] & 128) || length == 127 ||
            (p.opcode != 0 && p.opcode != 2 && p.opcode != 8 && p.opcode != 9 &&
             p.opcode != 10))
          return false;
        if (length == 126) {
          length = unsigned(p.header[2]) * 256 + p.header[3];
          if (length < 126)
            return false;
        }
        if (length > HubBroker::Packet + 5 ||
            (p.opcode >= 8 && (!p.fin || length > 125)) ||
            (p.opcode == 0 && !p.fragment) || (p.opcode == 2 && p.fragment))
          return false;
        if (p.opcode == 2 && !p.fin)
          p.fragment = true;
        if (p.opcode == 0 && p.fin)
          p.fragment = false;
        memcpy(p.mask, p.header + need - 4, 4);
        p.headerSize = 0;
        p.remaining = length;
        p.offset = 0;
        p.controlSize = 0;
        if (!length) {
          if (p.opcode == 8) {
            frame(i, 8, nullptr, 0);
            // WebSocket Close ends the transport; only MQTT DISCONNECT
            // suppresses a live session's will.
            broker.disconnect(i);
            return true;
          }
          if (p.opcode == 9 && !frame(i, 10, nullptr, 0))
            return false;
          continue;
        }
      }
      size_t take = std::min<size_t>(p.remaining, n - at);
      for (size_t j = 0; j < take; j++)
        data[at + j] ^= p.mask[(p.offset + j) % 4];
      if (p.opcode >= 8) {
        if (take > sizeof(p.control) - p.controlSize)
          return false;
        memcpy(p.control + p.controlSize, data + at, take);
        p.controlSize += take;
      }
      else if (!broker.receive(i, data + at, take, clock))
        return false;
      at += take;
      p.offset += take;
      p.remaining -= take;
      if (!p.remaining) {
        if (p.opcode == 8) {
          frame(i, 8, p.control, p.controlSize);
          broker.disconnect(i);
          return true;
        }
        if (p.opcode == 9 &&
            !frame(i, 10, p.control, p.controlSize))
          return false;
      }
    }
    return true;
  }
};
} // namespace iotcloud
