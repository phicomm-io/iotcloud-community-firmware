export const userKey = username => "u_" + [...new TextEncoder().encode(username)].map(b => b.toString(16).padStart(2,"0")).join("");
export const deviceKey = d => d.namespace === "account" ? `account:${d.username}:${d.mac}` : d.mac;
export const modelName = (model) => (model === "tc1" ? "tc1(a1)" : model);
export function normalizeMAC(value) {
  const s = String(value || "").trim();
  if (
    !/^(?:[a-f\d]{12}|(?:[a-f\d]{2}:){5}[a-f\d]{2}|(?:[a-f\d]{2}-){5}[a-f\d]{2})$/i.test(
      s,
    )
  )
    throw new Error("MAC 地址格式不正确");
  return s.replace(/[:-]/g, "").toLowerCase();
}
export function deviceEntry(value) {
  const d = typeof value === "string" ? { mac: value, version: "v1" } : value;
  const mac = normalizeMAC(d.mac);
  const model = d.model || "tc1";
  const version = d.version || (model === "tc1" ? "v2" : "community");
  if (
    !["tc1", "m1", "a1"].includes(model) ||
    !(model === "tc1"
      ? ["v1", "v2"].includes(version)
      : version === "community")
  )
    throw new Error("型号或协议不正确");
  if (d.namespace && (d.namespace !== "account" || !d.username)) throw new Error("账号主题缺少归属");
  return { mac, model, version, ...(d.namespace ? { namespace: d.namespace, username: d.username } : {}), ...(d.status ? { status: d.status } : {}) };
}
export function localDevices(storage = localStorage) {
  const list = JSON.parse(
    storage.getItem("phicomm_devices") ||
      storage.getItem("ztc1_devices") ||
      "[]",
  );
  if (!Array.isArray(list)) throw new Error("设备列表格式不正确");
  const unique = new Map();
  for (const raw of list) {
    const d = deviceEntry(raw);
    unique.set(deviceKey(d), d);
  }
  return [...unique.values()];
}
export function topics(d) {
  if (d.namespace === "account") return [`phicomm/${d.model}/${userKey(d.username)}/${d.mac}/#`, `homeassistant/+/${userKey(d.username)}/+/config`];
  if (d.model !== "tc1")
    return [
      `phicomm/${d.model}/${d.mac}/#`,
      `homeassistant/+/${d.model}_${d.mac}/+/config`,
    ];
  // Old lists accepted colon-delimited identities; preserve their wire topics too.
  const ids = new Set([
    d.mac,
    d.mac.toUpperCase(),
    d.mac.match(/../g).join(":"),
    d.mac.match(/../g).join(":").toUpperCase(),
  ]);
  return [...ids].flatMap((id) =>
    d.version === "v1"
      ? [
          `device/ztc1/${id}/sensor`,
          `device/ztc1/${id}/state`,
          `device/ztc1/${id}/availability`,
        ]
      : ["switch", "sensor", "button"].map(
          (type) => `homeassistant/${type}/${id}/+/#`,
        ),
  );
}
const merge = (state, data) => {
  for (const [key, value] of Object.entries(data))
    if (
      ![
        "__proto__",
        "constructor",
        "prototype",
        "mac",
        "model",
        "version",
        "status",
        "username",
        "namespace",
      ].includes(key)
    )
      state[key] = value;
};
const json = (value) => {
  try {
    const v = JSON.parse(value);
    return v && typeof v === "object" && !Array.isArray(v) ? v : null;
  } catch {
    return null;
  }
};
export function applyMessage(d, state, topic, message, now = Date.now()) {
  if (d.namespace === "account") {
    const p = topic.split("/"), key = userKey(d.username);
    if (p[0] === "homeassistant") {
      const prefix = `${d.model}_${d.mac}_`;
      if (p.length !== 5 || p[2] !== key || !p[3].startsWith(prefix)) return false;
      topic = `homeassistant/${p[1]}/${d.model === "tc1" ? d.mac : `${d.model}_${d.mac}`}/${p[3].slice(prefix.length)}/${p[4]}`;
    } else {
      if (p[0] !== "phicomm" || p[1] !== d.model || p[2] !== key || p[3] !== d.mac) return false;
      const leaf = p.slice(4).join("/");
      if (d.model === "tc1") {
        if (leaf === "availability") { state.available = String(message) === "online"; return true; }
        if (leaf === "sensor") topic = `homeassistant/sensor/${d.mac}/power/state`;
        else if (/^\w+\/state$/.test(leaf)) topic = `homeassistant/switch/${d.mac}/${leaf}`;
        else return false;
      } else topic = `phicomm/${d.model}/${d.mac}/${leaf}`;
    }
  }
  const p = topic.split("/"),
    raw = String(message);
  let mac;
  try {
    mac = normalizeMAC(
      p[0] === "homeassistant" ? p[2].replace(/^(?:m1|a1)_/, "") : p[2],
    );
  } catch {
    return false;
  }
  if (mac !== d.mac) return false;
  if (p[0] === "homeassistant") {
    if (
      p.length !== 5 ||
      (d.model === "tc1" ? p[2].includes("_") : !p[2].startsWith(`${d.model}_`))
    )
      return false;
    if (p[4] === "config") {
      const data = json(raw);
      if (!data) return false;
      if (data.device?.name) state.deviceName = data.device.name;
      if (data.name) (state.entityNames ||= {})[p[3]] = data.name;
    } else if (d.model === "tc1" && d.version === "v2" && p[4] === "state") {
      if (p[1] === "sensor") {
        const data = json(raw);
        if (data) merge(state, data);
      } else {
        const m = raw
          .trim()
          .match(/^set (socket|\w+) ([a-f\d:]+) (?:(\d) )?([01])$/i);
        if (!m) return false;
        try {
          if (normalizeMAC(m[2]) !== d.mac) return false;
        } catch {
          return false;
        }
        state[m[1] === "socket" ? `socket_${m[3]}` : m[1]] = Number(m[4]);
      }
    }
    return true;
  }
  if (d.model === "tc1") {
    if (p[0] !== "device" || p[1] !== "ztc1" || d.version !== "v1")
      return false;
    if (p[3] === "availability")
      state.available = ["online", "1", "true"].includes(raw);
    else {
      const data = json(raw);
      if (!data) return false;
      delete data.mac;
      merge(state, data);
    }
  } else {
    if (p[0] !== "phicomm" || p[1] !== d.model) return false;
    const leaf = p.slice(3).join("/");
    if (leaf === "availability") {
      state.available = raw === "online";
      if (!state.available && d.model === "m1") state.sensorFresh = false;
    } else if (d.model === "m1" && leaf === "sensor_availability")
      state.sensorFresh = raw === "online";
    else if (d.model === "m1" && leaf === "sensor") {
      const data = json(raw);
      if (!data) return false;
      merge(state, data);
      state.sensorAt = now;
      state.sensorFresh = true;
    } else if (leaf === "light/state") {
      const data = json(raw);
      if (!data) return false;
      state.light = data;
    } else if (leaf.endsWith("/state"))
      state[leaf.slice(0, -6).replaceAll("/", "_")] = raw;
  }
  return true;
}
export function command(d, cmd) {
  if (d.namespace === "account") {
    const result = command({ ...d, namespace: undefined }, cmd);
    result.topic = `phicomm/${d.model}/${userKey(d.username)}/${d.mac}/${d.model === "tc1" ? "set" : cmd.leaf}`;
    return result;
  }
  if (d.model === "tc1") {
    const mac = d.version === "v2" ? d.mac.toUpperCase() : d.mac;
    return {
      topic: `device/ztc1/${mac}/set`,
      payload:
        d.version === "v2"
          ? String(cmd.payload).replace(
              /\b(?:[a-f\d]{12}|(?:[a-f\d]{2}:){5}[a-f\d]{2})\b/gi,
              mac,
            )
          : JSON.stringify({ ...cmd, mac }),
    };
  }
  const allowed =
    d.model === "m1"
      ? ["brightness/set"]
      : [
          "fan/set",
          "fan/percentage/set",
          "light/set",
          "child_lock/set",
          "led_always_on/set",
        ];
  if (!allowed.includes(cmd.leaf)) throw new Error("不支持的命令");
  return {
    topic: `phicomm/${d.model}/${d.mac}/${cmd.leaf}`,
    payload:
      typeof cmd.value === "object"
        ? JSON.stringify(cmd.value)
        : String(cmd.value),
  };
}
