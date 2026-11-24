static const IPAddress g3PortalAddress(192, 168, 0, 1);

static size_t g3PortalDnsReply(uint8_t *packet, size_t size, size_t capacity) {
  if (size < 17 || size > capacity || (packet[2] & 0xf8) || packet[4] || packet[5] != 1)
    return 0;
  size_t end = 12;
  while (end < size && packet[end]) {
    const size_t label = packet[end];
    if (label > 63 || label >= size - end || end + label >= 267)
      return 0;
    end += label + 1;
  }
  if (size - end < 5)
    return 0;
  ++end;
  const bool ipv4 = !packet[end] && (packet[end + 1] == 1 || packet[end + 1] == 255) &&
                    !packet[end + 2] && packet[end + 3] == 1;
  end += 4;
  const uint8_t answer[] = {0xc0, 12, 0, 1, 0, 1, 0, 0, 0, 0, 0, 4, 192, 168, 0, 1};
  if (ipv4 && capacity - end < sizeof(answer))
    return 0;
  packet[2] = 0x84 | (packet[2] & 1);
  packet[3] = 0;
  memset(packet + 6, 0, 6);
  if (ipv4) {
    packet[7] = 1;
    memcpy(packet + end, answer, sizeof(answer));
    end += sizeof(answer);
  }
  return end;
}

static void pollG3PortalDns(uint32_t now) {
  static int fd = -1;
  static uint32_t nextBind = 0;
  const bool available = (WiFi.getMode() & WIFI_AP) && WiFi.softAPIP() == g3PortalAddress;
  if (!available) {
    if (fd >= 0) {
      close(fd);
      fd = -1;
    }
    nextBind = now;
    return;
  }
  if (fd < 0 && due(now, nextBind)) {
    nextBind = now + 1000;
    fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd >= 0) {
      sockaddr_in local = {};
      local.sin_family = AF_INET;
      local.sin_port = htons(53);
      local.sin_addr.s_addr = uint32_t(g3PortalAddress);
      if (bind(fd, reinterpret_cast<sockaddr *>(&local), sizeof(local)) != 0) {
        close(fd);
        fd = -1;
      }
    }
  }
  if (fd < 0)
    return;

  uint8_t packet[512];
  sockaddr_in peer = {};
  socklen_t peerLength = sizeof(peer);
  const int received = recvfrom(fd, packet, sizeof(packet), MSG_DONTWAIT,
                                reinterpret_cast<sockaddr *>(&peer), &peerLength);
  if (received <= 0 || (ntohl(peer.sin_addr.s_addr) >> 8) != 0xc0a800)
    return;
  const size_t length = g3PortalDnsReply(packet, received, sizeof(packet));
  if (length)
    sendto(fd, packet, length, MSG_DONTWAIT, reinterpret_cast<sockaddr *>(&peer), peerLength);
}

static esp_err_t g3WifiPortalRedirect(httpd_req_t *req, httpd_err_code_t error) {
  sockaddr_storage local = {};
  socklen_t length = sizeof(local);
  uint32_t address = 0;
  if (getsockname(httpd_req_to_sockfd(req), reinterpret_cast<sockaddr *>(&local), &length) == 0) {
    if (local.ss_family == AF_INET)
      address = reinterpret_cast<const sockaddr_in &>(local).sin_addr.s_addr;
#if LWIP_IPV6
    else if (local.ss_family == AF_INET6) {
      const auto &ip = reinterpret_cast<const sockaddr_in6 &>(local).sin6_addr;
      const uint8_t mappedPrefix[] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
      if (!memcmp(ip.s6_addr, mappedPrefix, sizeof(mappedPrefix)))
        memcpy(&address, &ip.s6_addr[12], sizeof(address));
    }
#endif
  }
  if (req->method != HTTP_GET || address != uint32_t(g3PortalAddress) ||
      !(WiFi.getMode() & WIFI_AP) || !strncmp(req->uri, "/api/", 5))
    return httpd_resp_send_err(req, error, nullptr);
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", "http://192.168.0.1/#settings/wifi");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, nullptr, 0);
}
