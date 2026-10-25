/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <list>
#include <string>
#include <vector>

namespace iotcloud {
// MQTT 3.1.1: QoS 0/1 publishers, QoS 0 subscriptions. All sessions are clean.
// Limits cover shared payload storage as well as individual clients.
class HubBroker {
public:
  // Topic names and filters are UTF-8 bytes, excluding their MQTT length field.
  // Match MQTT_TOPIC_MAX without increasing any shared storage budget.
  static constexpr size_t Clients = 8, Packet = 8192, Topic = 512,
                          Subscriptions = 24;
  static constexpr size_t ReceiveBudget = 24576, RetainedBudget = 49152,
                          WillBudget = 4096, SubscriptionBudget = 8192;
  enum class Origin { Local, Upstream, Bemfa, Internal };
  using Send = std::function<bool(unsigned, const uint8_t *, size_t)>;
  using Close = std::function<void(unsigned)>;
  using Authenticate =
      std::function<bool(const std::string &, const std::string &)>;
  using Authorize = std::function<bool(const std::string &, bool)>;
  using Event = std::function<void(const std::string &, const std::string &,
                                   bool, Origin)>;
  std::function<bool(size_t)> allocate;
  std::function<bool(unsigned, size_t)> writable;
  Send send;
  // A transport may enqueue the MQTT header/topic/payload directly into its
  // bounded transmit buffer, without two extra full-payload packet copies.
  std::function<bool(unsigned, const uint8_t *, size_t,
                     const std::string &, const std::string &)> sendPublish;
  Close close;
  Authenticate authenticate;
  Authorize authorize;
  Event event;
  unsigned refused = 0, dropped = 0;

  static bool utf8(const std::string &s) {
    for (size_t i = 0; i < s.size();) {
      uint32_t c = uint8_t(s[i++]);
      unsigned n = 0;
      uint32_t minimum = 0;
      if (c >= 0xc2 && c <= 0xdf) {
        n = 1;
        c &= 31;
        minimum = 0x80;
      } else if (c >= 0xe0 && c <= 0xef) {
        n = 2;
        c &= 15;
        minimum = 0x800;
      } else if (c >= 0xf0 && c <= 0xf4) {
        n = 3;
        c &= 7;
        minimum = 0x10000;
      } else if (c >= 0x80)
        return false;
      if (i + n > s.size())
        return false;
      while (n--) {
        uint8_t x = s[i++];
        if ((x & 0xc0) != 0x80)
          return false;
        c = c * 64 + (x & 63);
      }
      if (c < minimum || !c || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff) ||
          (c >= 0xfdd0 && c <= 0xfdef) || (c & 0xffff) == 0xfffe ||
          (c & 0xffff) == 0xffff)
        return false;
    }
    return true;
  }
  static bool topicValid(const std::string &s, bool filter = false) {
    if (s.empty() || s.size() > Topic || !utf8(s))
      return false;
    for (size_t i = 0; i < s.size(); i++) {
      if (s[i] == '#' &&
          (!filter || i + 1 != s.size() || (i && s[i - 1] != '/')))
        return false;
      if (s[i] == '+' && (!filter || (i && s[i - 1] != '/') ||
                          (i + 1 < s.size() && s[i + 1] != '/')))
        return false;
    }
    return true;
  }
  static bool matches(const std::string &filter, const std::string &topic) {
    if (filter.empty() || topic.empty())
      return false;
    if (!topic.empty() && topic[0] == '$' &&
        (filter[0] == '#' || filter[0] == '+'))
      return false;
    size_t f = 0, t = 0;
    while (true) {
      if (f < filter.size() && filter[f] == '#')
        return true;
      size_t fe = filter.find('/', f), te = topic.find('/', t);
      if (fe == std::string::npos)
        fe = filter.size();
      if (te == std::string::npos)
        te = topic.size();
      if (filter.compare(f, fe - f, "+") &&
          filter.substr(f, fe - f) != topic.substr(t, te - t))
        return false;
      if (te == topic.size())
        return fe == filter.size() || filter.substr(fe) == "/#";
      if (fe == filter.size())
        return false;
      f = fe + 1;
      t = te + 1;
    }
  }
  bool open(unsigned slot, uint32_t now) {
    if (slot >= Clients || clients[slot].used)
      return false;
    clients[slot] = Client();
    clients[slot].used = true;
    clients[slot].at = clients[slot].partialAt = now;
    return true;
  }
  bool active(unsigned slot) const {
    return slot < Clients && clients[slot].used;
  }
  bool connected(unsigned slot) const {
    return active(slot) && clients[slot].connected;
  }
  unsigned count() const {
    unsigned n = 0;
    for (const auto &c : clients)
      n += c.used && c.connected;
    return n;
  }
  size_t retainedBytes() const {
    size_t n = 0;
    for (const auto &r : retained)
      n += r.topic.size() + r.body.size();
    return n;
  }
  size_t retainedCount() const { return retained.size(); }
  void reset() {
    // Account/password changes invalidate every connection, will and retained
    // ACL.
    for (unsigned i = 0; i < Clients; i++)
      disconnect(i, true);
    retained.clear();
  }
  void disconnect(unsigned slot, bool graceful = false) {
    if (!active(slot))
      return;
    auto &c = clients[slot];
    std::string topic, body;
    bool retain = c.willRetain;
    if (c.connected && !graceful) {
      topic = std::move(c.willTopic);
      body = std::move(c.willBody);
    }
    c = Client();
    if (close)
      close(slot);
    if (!topic.empty())
      publish(topic, body, retain, Origin::Local);
  }
  void tick(uint32_t now) {
    for (unsigned i = 0; i < Clients; i++)
      if (active(i)) {
        auto &c = clients[i];
        if ((!c.connected && uint32_t(now - c.at) > 5000) ||
            (!c.input.empty() && uint32_t(now - c.partialAt) > 5000) ||
            (c.keepalive &&
             uint32_t(now - c.at) > uint32_t(c.keepalive) * 1500)) {
          disconnect(i);
          continue;
        }
        for (unsigned n = 0; n < 2 && connected(i) && !c.replay.empty(); n++) {
          const uint32_t id = c.replay.front();
          auto saved =
              std::find_if(retained.begin(), retained.end(),
                           [&](const Retained &r) { return r.id == id; });
          if (saved == retained.end()) {
            c.replay.erase(c.replay.begin());
            continue;
          }
          if (writable &&
              !writable(i, saved->topic.size() + saved->body.size() + 7))
            break;
          c.replay.erase(c.replay.begin());
          bool subscribed = false;
          for (const auto &filter : c.subscriptions)
            if (matches(filter, saved->topic))
              subscribed = true;
          if (subscribed && allowed(saved->topic, false))
            deliver(i, saved->topic, saved->body, true);
        }
      }
  }
  bool receive(unsigned slot, const uint8_t *data, size_t n, uint32_t now) {
    if (!active(slot) || !n)
      return false;
    auto &c = clients[slot];
    while (n && active(slot)) {
      size_t buffered = 0;
      for (const auto &x : clients)
        buffered += x.input.size();
      size_t take = std::min(
          n, std::min(Packet + 5 - c.input.size(), ReceiveBudget - buffered));
      const size_t size = c.input.size() + take;
      // Only replace the actual input buffer here. A small packet must not
      // require a contiguous maximum-size packet buffer on every fragment.
      if (!take || (size > c.input.capacity() && allocate && !allocate(size))) {
        ++refused;
        disconnect(slot);
        return false;
      }
      if (c.input.empty())
        c.partialAt = now;
      c.input.reserve(size);
      c.input.insert(c.input.end(), data, data + take);
      data += take;
      n -= take;
      while (active(slot) && c.input.size() >= 2) {
        size_t remaining = 0, multiplier = 1, header = 1;
        bool complete = false;
        for (unsigned i = 0; i < 4 && header < c.input.size(); i++) {
          uint8_t x = c.input[header++];
          remaining += (x & 127) * multiplier;
          multiplier *= 128;
          if (!(x & 128)) {
            complete = true;
            break;
          }
        }
        if (!complete) {
          if (header == 5) {
            ++refused;
            disconnect(slot);
            return false;
          }
          break;
        }
        if (remaining > Packet || header + remaining > Packet + 5) {
          ++refused;
          disconnect(slot);
          return false;
        }
        if (c.input.size() < header + remaining)
          break;
        const uint8_t flags = c.input[0];
        const size_t tail = c.input.size() - header - remaining;
        // The input allocation remains live while the tail and parsed strings
        // are copied. Subscription handling also clones bounded filter vectors;
        // their largest replacement is independent of a tiny request's length.
        size_t working = remaining + tail + 512;
        if (flags == 0x82 || flags == 0xa2)
          working = std::max(working, Subscriptions * 2 * sizeof(std::string) + 512);
        if (allocate && !allocate(working)) {
          ++refused;
          disconnect(slot);
          return false;
        }
        // Own the packet while callbacks may reset or disconnect its client.
        std::vector<uint8_t> body;
        body.swap(c.input);
        c.input.assign(body.begin() + header + remaining, body.end());
        body.erase(body.begin() + header + remaining, body.end());
        body.erase(body.begin(), body.begin() + header);
        c.at = now;
        c.partialAt = now;
        if (!packet(slot, flags, body)) {
          ++refused;
          disconnect(slot);
          return false;
        }
      }
    }
    return active(slot);
  }
  bool publish(const std::string &topic, const std::string &body,
               bool retain = false, Origin origin = Origin::Internal) {
    if ((allocate && !allocate(topic.size() + body.size() * (sendPublish ? 1 : 2) + 512)) ||
        !topicValid(topic) || topic.size() + body.size() + 4 > Packet ||
        !allowed(topic, true))
      return false;
    // Retained command snapshots must never become executable commands.
    if (retain && (ends(topic, "/command") || ends(topic, "/set")))
      return false;
    if (retain) {
      auto found =
          std::find_if(retained.begin(), retained.end(),
                       [&](const Retained &r) { return r.topic == topic; });
      const size_t old = found == retained.end()
                             ? 0
                             : found->topic.size() + found->body.size();
      if (!body.empty() &&
          (retainedBytes() - old + topic.size() + body.size() >
               RetainedBudget ||
           (found == retained.end() && retained.size() >= 128))) {
        ++refused;
        return false;
      }
      if (body.empty()) {
        if (found != retained.end())
          retained.erase(found);
      } else if (found != retained.end())
        std::string(body).swap(found->body);
      else
        retained.push_back({topic, body, ++retainedSequence});
    }
    for (unsigned i = 0; i < Clients; i++)
      if (connected(i) && allowed(topic, false)) {
        for (const auto &filter : clients[i].subscriptions)
          if (matches(filter, topic)) {
            deliver(i, topic, body, false);
            break;
          }
      }
    if (event)
      event(topic, body, retain, origin);
    return true;
  }

private:
  struct Client {
    bool used = false, connected = false, willRetain = false;
    uint16_t keepalive = 0;
    uint32_t at = 0, partialAt = 0;
    std::string id, willTopic, willBody;
    std::vector<uint8_t> input;
    std::vector<std::string> subscriptions;
    std::vector<uint32_t> replay;
  };
  struct Retained {
    std::string topic, body;
    uint32_t id;
  };
  uint32_t retainedSequence = 0;
  std::array<Client, Clients> clients;
  // One fixed-size node per retained message avoids a whole-array replacement
  // while all payloads are live. publish() budgets its node with the payload.
  std::list<Retained> retained;
  struct Reader {
    const std::vector<uint8_t> &data;
    size_t pos = 0;
    bool ok = true;
    explicit Reader(const std::vector<uint8_t> &input) : data(input) {}
    uint8_t byte() {
      if (pos >= data.size()) {
        ok = false;
        return 0;
      }
      return data[pos++];
    }
    uint16_t word() {
      uint16_t n = byte();
      return n * 256 + byte();
    }
    std::string text(bool validate = true) {
      size_t n = word();
      if (!ok || pos + n > data.size()) {
        ok = false;
        return {};
      }
      std::string s(data.begin() + pos, data.begin() + pos + n);
      pos += n;
      if (validate && !utf8(s))
        ok = false;
      return s;
    }
    bool done() const { return ok && pos == data.size(); }
  };
  static bool ends(const std::string &s, const char *suffix) {
    size_t n = std::char_traits<char>::length(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
  }
  bool allowed(const std::string &topic, bool write) const {
    return authorize && authorize(topic, write);
  }
  bool output(unsigned slot, uint8_t flags, const std::vector<uint8_t> &body) {
    if (!active(slot))
      return false;
    std::vector<uint8_t> bytes;
    bytes.reserve(body.size() + 5);
    bytes.push_back(flags);
    size_t n = body.size();
    do {
      uint8_t b = n % 128;
      n /= 128;
      bytes.push_back(b | (n ? 128 : 0));
    } while (n);
    bytes.insert(bytes.end(), body.begin(), body.end());
    if (!send || !send(slot, bytes.data(), bytes.size())) {
      ++dropped;
      disconnect(slot);
      return false;
    }
    return true;
  }
  void deliver(unsigned slot, const std::string &topic, const std::string &body,
               bool replay) {
    if (sendPublish) {
      uint8_t header[7] = {uint8_t(0x30 | (replay ? 1 : 0))};
      size_t used = 1, remaining = topic.size() + body.size() + 2;
      do {
        uint8_t byte = remaining % 128;
        remaining /= 128;
        header[used++] = byte | (remaining ? 128 : 0);
      } while (remaining);
      header[used++] = topic.size() >> 8;
      header[used++] = topic.size() & 255;
      if (!active(slot) || !sendPublish(slot, header, used, topic, body)) {
        ++dropped;
        disconnect(slot);
      }
      return;
    }
    std::vector<uint8_t> bytes;
    bytes.reserve(topic.size() + body.size() + 2);
    bytes.push_back(topic.size() >> 8);
    bytes.push_back(topic.size() & 255);
    bytes.insert(bytes.end(), topic.begin(), topic.end());
    bytes.insert(bytes.end(), body.begin(), body.end());
    output(slot, 0x30 | (replay ? 1 : 0), bytes);
  }
  bool connect(unsigned slot, Reader &r) {
    auto protocol = r.text();
    auto level = r.byte(), flags = r.byte();
    auto keepalive = r.word();
    auto id = r.text();
    bool valid = protocol == "MQTT" && level == 4 && !(flags & 1) &&
                 !id.empty() && id.size() <= 128 && ((flags & 2) != 0) &&
                 ((flags & 0xc0) == 0xc0) &&
                 (!(flags & 4) ? !(flags & 0x38) : ((flags >> 3) & 3) <= 1);
    std::string willTopic, willBody;
    if (flags & 4) {
      willTopic = r.text();
      willBody = r.text(false);
    }
    auto username = r.text(), password = r.text(false);
    valid =
        valid && r.done() && username.size() <= 63 && password.size() <= 128 &&
        (!(flags & 4) || (topicValid(willTopic) && allowed(willTopic, true) &&
                          willBody.size() <= 1024 &&
                          (!(flags & 32) || (!ends(willTopic, "/command") &&
                                             !ends(willTopic, "/set")))));
    uint8_t refusal = valid ? 0 : 2;
    if (!refusal && (!authenticate || !authenticate(username, password)))
      refusal = 4;
    size_t wills = willTopic.size() + willBody.size();
    for (const auto &c : clients)
      wills += c.willTopic.size() + c.willBody.size();
    if (wills > WillBudget)
      refusal = 3;
    if (refusal) {
      output(slot, 0x20, {0, refusal});
      return false;
    }
    for (unsigned i = 0; i < Clients; i++)
      if (i != slot && connected(i) && clients[i].id == id)
        disconnect(i);
    auto &c = clients[slot];
    c.id = std::move(id);
    c.keepalive = keepalive;
    c.connected = true;
    c.willTopic = std::move(willTopic);
    c.willBody = std::move(willBody);
    c.willRetain = flags & 32;
    return output(slot, 0x20, {0, 0});
  }
  bool packet(unsigned slot, uint8_t flags, const std::vector<uint8_t> &body) {
    Reader r{body};
    auto &c = clients[slot];
    unsigned type = flags >> 4;
    if (!c.connected)
      return flags == 0x10 && connect(slot, r);
    if (type == 3) {
      unsigned qos = (flags >> 1) & 3;
      if (qos > 1 || (!qos && (flags & 8)))
        return false;
      auto topic = r.text();
      uint16_t id = qos ? r.word() : 0;
      if (!r.ok || (qos && !id) || !topicValid(topic) || !allowed(topic, true))
        return false;
      std::string payload(body.begin() + r.pos, body.end());
      if (!publish(topic, payload, flags & 1, Origin::Local))
        return false;
      return !qos || output(slot, 0x40, {uint8_t(id >> 8), uint8_t(id)});
    }
    if (flags == 0x82) {
      uint16_t id = r.word();
      if (!id || r.pos == body.size())
        return false;
      auto next = c.subscriptions;
      std::vector<uint8_t> ack = {uint8_t(id >> 8), uint8_t(id)};
      std::vector<std::string> accepted;
      while (r.ok && r.pos < body.size()) {
        auto filter = r.text();
        uint8_t qos = r.byte();
        if (qos > 2 || !topicValid(filter, true))
          return false;
        bool exists = std::find(next.begin(), next.end(), filter) != next.end();
        size_t bytes = filter.size();
        for (unsigned i = 0; i < Clients; i++)
          if (i != slot)
            for (const auto &f : clients[i].subscriptions)
              bytes += f.size();
        for (const auto &f : next)
          bytes += f.size();
        if (!exists &&
            (next.size() >= Subscriptions || bytes > SubscriptionBudget)) {
          ack.push_back(0x80);
          continue;
        }
        if (!exists)
          next.push_back(filter);
        // Repeating an existing filter still gets its own SUBACK, but must not
        // allocate an unbounded list of duplicate replay filters.
        if (std::find(accepted.begin(), accepted.end(), filter) == accepted.end())
          accepted.push_back(filter);
        ack.push_back(0);
      }
      if (!r.done())
        return false;
      c.subscriptions = std::move(next);
      if (!output(slot, 0x90, ack))
        return false;
      // Queue identifiers, not payload copies. tick() delivers retained data in
      // bounded batches after the socket drains, including large HA discovery
      // sets.
      c.replay.erase(std::remove_if(c.replay.begin(), c.replay.end(),
                                    [&](uint32_t id) {
                                      return std::none_of(
                                          retained.begin(), retained.end(),
                                          [&](const Retained &saved) {
                                            return saved.id == id;
                                          });
                                    }),
                     c.replay.end());
      for (const auto &saved : retained) {
        bool match = false;
        for (const auto &filter : accepted)
          if (matches(filter, saved.topic))
            match = true;
        if (match && allowed(saved.topic, false) &&
            std::find(c.replay.begin(), c.replay.end(), saved.id) ==
                c.replay.end())
          c.replay.push_back(saved.id);
      }
      return true;
    }
    if (flags == 0xa2) {
      uint16_t id = r.word();
      if (!id || r.pos == body.size())
        return false;
      auto next = c.subscriptions;
      while (r.ok && r.pos < body.size()) {
        auto filter = r.text();
        if (!topicValid(filter, true))
          return false;
        next.erase(std::remove(next.begin(), next.end(), filter), next.end());
      }
      if (!r.done())
        return false;
      c.subscriptions = std::move(next);
      return output(slot, 0xb0, {uint8_t(id >> 8), uint8_t(id)});
    }
    if (flags == 0xc0 && body.empty())
      return output(slot, 0xd0, {});
    if (flags == 0xe0 && body.empty()) {
      disconnect(slot, true);
      return true;
    }
    return false;
  }
};
} // namespace iotcloud
