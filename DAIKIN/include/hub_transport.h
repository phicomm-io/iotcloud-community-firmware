/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "hub_broker.h"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

namespace iotcloud {
// The application loop owns sockets, protocol state and all callbacks.
class HubTransport {
public:
  static constexpr size_t TxBudget = 32768, HttpLimit = 2048;
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
          broker.open(slot, now);
        }
      }
    uint8_t buffer[512];
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
          if (p.tx.capacity() > p.tx.size() + 1024)
            p.tx.shrink_to_fit();
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
  struct Peer {
    int fd = -1;
    bool ws = false, upgraded = false, closing = false, fragment = false;
    uint32_t at = 0, closedAt = 0;
    std::vector<uint8_t> tx, header;
    std::string http, control;
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
  static std::string lower(std::string s) {
    for (auto &c : s)
      if (c >= 'A' && c <= 'Z')
        c += 32;
    return s;
  }
  static std::string field(const std::string &http, const std::string &name) {
    size_t at = http.find("\r\n");
    while (at != std::string::npos) {
      at += 2;
      size_t end = http.find("\r\n", at);
      if (end == std::string::npos || end == at)
        break;
      size_t colon = http.find(':', at);
      if (colon < end && lower(http.substr(at, colon - at)) == name) {
        size_t start = colon + 1;
        while (start < end && (http[start] == ' ' || http[start] == '\t'))
          start++;
        while (end > start && (http[end - 1] == ' ' || http[end - 1] == '\t'))
          end--;
        return http.substr(start, end - start);
      }
      at = end;
    }
    return {};
  }
  static bool token(const std::string &value, const char *wanted) {
    size_t at = 0;
    while (at < value.size()) {
      size_t end = value.find(',', at);
      if (end == std::string::npos)
        end = value.size();
      auto part = value.substr(at, end - at);
      while (!part.empty() && part.front() == ' ')
        part.erase(0, 1);
      while (!part.empty() && part.back() == ' ')
        part.pop_back();
      if (lower(part) == wanted)
        return true;
      at = end + 1;
    }
    return false;
  }
  bool upgrade(unsigned i) {
    auto &p = peers[i];
    auto key = field(p.http, "sec-websocket-key");
    if (p.http.compare(0, 20, "GET /mqtt HTTP/1.1\r\n") ||
        field(p.http, "sec-websocket-version") != "13" ||
        !token(field(p.http, "connection"), "upgrade") ||
        lower(field(p.http, "upgrade")) != "websocket" ||
        !token(field(p.http, "sec-websocket-protocol"), "mqtt") ||
        key.size() != 24 || !websocketAccept)
      return false;
    std::string answer = websocketAccept(key);
    if (answer.empty())
      return false;
    std::string response =
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: "
        "Upgrade\r\nSec-WebSocket-Protocol: mqtt\r\nSec-WebSocket-Accept: " +
        answer + "\r\n\r\n";
    if (!raw(i, (const uint8_t *)response.data(), response.size()))
      return false;
    p.upgraded = true;
    p.http.clear();
    return true;
  }
  bool websocket(unsigned i, uint8_t *data, size_t n) {
    auto &p = peers[i];
    size_t at = 0;
    while (at < n && !p.closing) {
      if (!p.upgraded) {
        p.http.push_back(data[at++]);
        if (p.http.size() > HttpLimit)
          return false;
        if (p.http.size() >= 4 &&
            p.http.compare(p.http.size() - 4, 4, "\r\n\r\n") == 0 &&
            !upgrade(i))
          return false;
        continue;
      }
      if (!p.remaining) {
        p.header.push_back(data[at++]);
        if (p.header.size() < 2)
          continue;
        unsigned length = p.header[1] & 127;
        size_t need = 2 + (length == 126 ? 2 : length == 127 ? 8 : 0) + 4;
        if (p.header.size() < need)
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
        memcpy(p.mask, p.header.data() + need - 4, 4);
        p.header.clear();
        p.remaining = length;
        p.offset = 0;
        p.control.clear();
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
      if (p.opcode >= 8)
        p.control.append((const char *)data + at, take);
      else if (!broker.receive(i, data + at, take, clock))
        return false;
      at += take;
      p.offset += take;
      p.remaining -= take;
      if (!p.remaining) {
        if (p.opcode == 8) {
          frame(i, 8, (const uint8_t *)p.control.data(), p.control.size());
          broker.disconnect(i);
          return true;
        }
        if (p.opcode == 9 &&
            !frame(i, 10, (const uint8_t *)p.control.data(), p.control.size()))
          return false;
      }
    }
    return true;
  }
};
} // namespace iotcloud
