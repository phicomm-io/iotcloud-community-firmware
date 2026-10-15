/* A1 group fan-out: one independent outstanding datagram per purifier. */
#ifdef FEATURE_DEVICE_A1
static struct {
  char ip[16];
  uint32_t sent, retry_at, request, boot, last_id[11];
  unsigned failures;
  unsigned successes;
  bool lan, joined;
  json_object *pending;
} group_members[8];
static void group_config_changed(const feature_config_t *old,
                                 const feature_config_t *next) {
  if (!strcmp(old->group, next->group) &&
      old->member_count == next->member_count &&
      !memcmp(old->members, next->members, sizeof(old->members)))
    return;
  for (unsigned i = 0; i < 8; i++)
    if (group_members[i].pending)
      json_object_put(group_members[i].pending);
  memset(group_members, 0, sizeof(group_members));
  memset(member_seen, 0, sizeof(member_seen));
}
static bool group_token_empty(const uint8_t t[16]) {
  unsigned n = 0;
  for (unsigned i = 0; i < 16; i++)
    n |= t[i];
  return n == 0;
}
static json_object *group_envelope(const feature_config_t *c, unsigned i,
                                   const char *op) {
  json_object *o = json_object_new_object();
  number(o, "v", 1);
  js(o, "src", LOCAL_ID);
  js(o, "dst", c->members[i].id);
  js(o, "group", c->group);
  js(o, "op", op);
  number(o, "boot", boot_id);
  number(o, "id", next_request());
  char token[33];
  token_text(c->group_token, token);
  js(o, "token", token);
  return o;
}
static void group_mqtt(unsigned i, const feature_config_t *c, json_object *o) {
  char topic[FEATURE_TOPIC_MAX];
  mqtt_account_t account=mqtt_snapshot();
  if (!mqtt_link_topic(&account,"a1",c->members[i].id,LOCAL_ID,"request",topic,sizeof(topic))) return;
  enqueue(topic, o, false);
}
static void group_send(const feature_config_t *c, unsigned i, json_object *o) {
  char ip[16];
  bool lan;
  lock();
  strcpy(ip, c->members[i].ip[0] ? c->members[i].ip : group_members[i].ip);
  lan = group_members[i].lan && !c->group_force_mqtt;
  unlock();
  if (lan && send_udp(ip, o)) {
    lock();
    json_object *old = group_members[i].pending;
    group_members[i].pending = json_object_get(o);
    group_members[i].request = integer(o, "id");
    group_members[i].sent = now_ms();
    unlock();
    if (old) {
      group_mqtt(i, c, old);
      json_object_put(old);
    }
  } else
    group_mqtt(i, c, o);
}
static bool group_input(json_object *o, bool lan, bool retained,
                        const char *ip) {
  const char *op = string(o, "op");
  if (strncmp(op, "group_", 6))
    return false;
  if (retained)
    return true;
  feature_config_t *c = malloc(sizeof(*c));
  if (!c)
    return true;
  feature_config_get(c);
  if (!c->group[0] || strcmp(c->group, string(o, "group")))
    goto done;
  unsigned i;
  for (i = 0; i < c->member_count; i++)
    if (!strcmp(c->members[i].id, string(o, "src")))
      break;
  if (i == c->member_count || !strcmp(string(o, "src"), LOCAL_ID))
    goto done;
  if (!strcmp(op, "group_join") && !lan) {
    uint8_t token[16];
    if (!token_read(string(o, "token"), token))
      goto done;
    if (strcmp(string(o, "src"), LOCAL_ID) < 0 ||
        group_token_empty(c->group_token)) {
      if (memcmp(c->group_token, token, 16)) {
        memcpy(c->group_token, token, 16);
        if (!install_config(c))
          goto done;
      }
    }
  }
  if (group_token_empty(c->group_token))
    goto done;
  char token[33];
  token_text(c->group_token, token);
  if (strcmp(token, string(o, "token")))
    goto done;
  lock();
  member_seen[i] = now_ms();
  if (ip && ip[0])
    strcpy(group_members[i].ip, ip);
  unlock();
  if (!strcmp(op, "group_ack")) {
    json_object *old = NULL;
    lock();
    group_members[i].joined = true;
    if (group_members[i].pending &&
        integer(o, "id") == group_members[i].request) {
      old = group_members[i].pending;
      group_members[i].pending = NULL;
      if (lan && ++group_members[i].successes >= 2)
        group_members[i].lan = true;
    }
    unlock();
    if (old)
      json_object_put(old);
    goto done;
  }
  unsigned slot = 10;
  if (!strcmp(op, "group_config")) {
    const char *d = string(o, "domain");
    const char *names[] = {"binding",  "linkage", "light",
                           "schedule", "sleep",   "time"};
    for (unsigned k = 0; k < 6; k++)
      if (!strcmp(d, names[k]))
        slot = k;
  }
  if (!strcmp(op, "group_control")) {
    const char *k = string(o, "kind");
    slot = !strcmp(k, "fan")          ? 6
           : !strcmp(k, "light")      ? 7
           : !strcmp(k, "child_lock") ? 8
                                      : 9;
  }
  bool duplicate;
  lock();
  if (group_members[i].boot != integer(o, "boot")) {
    group_members[i].boot = integer(o, "boot");
    memset(group_members[i].last_id, 0, sizeof(group_members[i].last_id));
  }
  duplicate = (int32_t)(integer(o, "id") - group_members[i].last_id[slot]) <= 0;
  if (!duplicate)
    group_members[i].last_id[slot] = integer(o, "id");
  unlock();
  bool ok = true;
  if (!duplicate) {
    json_object *b = json_object_object_get(o, "body");
    if (!strcmp(op, "group_control"))
      ok = a1_control_local(string(o, "kind"), b);
    else if (!strcmp(op, "group_config")) {
      const char *d = string(o, "domain");
      ok = b && config_decode(c, d, b);
      if (ok)
        ok = install_config(c);
    } else if (strcmp(op, "group_probe") && strcmp(op, "group_join"))
      ok = false;
  }
  if (ok) {
    json_object *r = group_envelope(c, i, "group_ack");
    number(r, "id", integer(o, "id"));
    if (lan)
      send_udp(ip, r);
    else
      group_mqtt(i, c, r);
    json_object_put(r);
  }
done:
  free(c);
  return true;
}
static void group_poll(const feature_config_t *c, uint32_t now) {
  if (!c->group[0])
    return;
  if (feature_is_coordinator() && group_token_empty(c->group_token)) {
    feature_config_t *next = malloc(sizeof(*next));
    if (next) {
      *next = *c;
      new_token(next->group_token);
      install_config(next);
      free(next);
    }
    return;
  }
  for (unsigned i = 0; i < c->member_count; i++) {
    if (!strcmp(c->members[i].id, LOCAL_ID))
      continue;
    json_object *expired = NULL;
    char ip[16];
    bool joined, lan, pending;
    uint32_t retry;
    lock();
    strcpy(ip, c->members[i].ip[0] ? c->members[i].ip : group_members[i].ip);
    if (group_members[i].pending &&
        (uint32_t)(now - group_members[i].sent) >= c->group_timeout_ms) {
      expired = group_members[i].pending;
      group_members[i].pending = NULL;
      group_members[i].lan = false;
      group_members[i].failures++;
      group_members[i].successes = 0;
      group_members[i].retry_at = now + c->group_recovery_seconds * 1000;
    }
    joined = group_members[i].joined;
    lan = group_members[i].lan && !c->group_force_mqtt;
    pending = group_members[i].pending != NULL;
    retry = group_members[i].retry_at;
    unlock();
    if (expired) {
      group_mqtt(i, c, expired);
      json_object_put(expired);
    }
    if (!joined && feature_is_coordinator() &&
        (uint32_t)(now - group_members[i].sent) >= 5000) {
      json_object *o = group_envelope(c, i, "group_join");
      group_mqtt(i, c, o);
      json_object_put(o);
      group_members[i].sent = now;
    }
    if (joined && !pending && ip[0] && !lan && !c->group_force_mqtt &&
        (retry == 0 || c->group_recovery) && (int32_t)(now - retry) >= 0) {
      json_object *o = group_envelope(c, i, "group_probe");
      if (send_udp(ip, o)) {
        lock();
        group_members[i].pending = json_object_get(o);
        group_members[i].request = integer(o, "id");
        group_members[i].sent = now;
        unlock();
      }
      json_object_put(o);
    }
  }
}
static bool group_manual_probe(const feature_config_t *c, const char *mac) {
  for (unsigned i = 0; i < c->member_count; i++)
    if (!strcmp(c->members[i].id, mac)) {
      lock();
      json_object *old = group_members[i].pending;
      group_members[i].pending = NULL;
      group_members[i].retry_at = 0;
      group_members[i].successes = 0;
      group_members[i].lan = false;
      unlock();
      if (old) {
        group_mqtt(i, c, old);
        json_object_put(old);
      }
      return true;
    }
  return false;
}
static void group_broadcast_config(const char *d, json_object *b) {
  feature_config_t *c = malloc(sizeof(*c));
  if (!c)
    return;
  feature_config_get(c);
  if (c->group[0])
    for (unsigned i = 0; i < c->member_count; i++)
      if (strcmp(c->members[i].id, LOCAL_ID)) {
        json_object *o = group_envelope(c, i, "group_config");
        js(o, "domain", d);
        json_object_object_add(o, "body", json_object_get(b));
        group_send(c, i, o);
        json_object_put(o);
      }
  free(c);
}
bool feature_group_control(const char *k, json_object *b) {
  feature_config_t *c = malloc(sizeof(*c));
  if (!c)
    return false;
  feature_config_get(c);
  bool group = c->group[0];
  if (group)
    for (unsigned i = 0; i < c->member_count; i++)
      if (strcmp(c->members[i].id, LOCAL_ID)) {
        json_object *o = group_envelope(c, i, "group_control");
        js(o, "kind", k);
        json_object_object_add(o, "body", json_object_get(b));
        group_send(c, i, o);
        json_object_put(o);
      }
  free(c);
  return group;
}
#else
static bool group_manual_probe(const feature_config_t *c, const char *m) {
  UNUSED_PARAMETER(c);
  UNUSED_PARAMETER(m);
  return false;
}
static void group_config_changed(const feature_config_t *old,
                                 const feature_config_t *next) {
  UNUSED_PARAMETER(old);
  UNUSED_PARAMETER(next);
}
static bool group_input(json_object *o, bool l, bool r, const char *i) {
  UNUSED_PARAMETER(o);
  UNUSED_PARAMETER(l);
  UNUSED_PARAMETER(r);
  UNUSED_PARAMETER(i);
  return false;
}
static void group_poll(const feature_config_t *c, uint32_t n) {
  UNUSED_PARAMETER(c);
  UNUSED_PARAMETER(n);
}
static void group_broadcast_config(const char *d, json_object *b) {
  UNUSED_PARAMETER(d);
  UNUSED_PARAMETER(b);
}
bool feature_group_control(const char *k, json_object *o) {
  UNUSED_PARAMETER(k);
  UNUSED_PARAMETER(o);
  return false;
}
#endif
