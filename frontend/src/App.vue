<script setup>
import {
  ref,
  reactive,
  computed,
  watch,
  onMounted,
  onBeforeUnmount,
} from "vue";
import mqtt from "mqtt";
import DeviceControl from "./components/DeviceControl.vue";
import CommunityControl from "./components/CommunityControl.vue";
import ImportExport from "./components/ImportExport.vue";
import { account, api, restoreAccount } from "./account";
import {
  modelName,
  deviceEntry,
  localDevices,
  topics,
  applyMessage,
  command,
} from "./deviceProtocol";
const ready = ref(false),
  list = ref([]),
  states = reactive({}),
  selected = ref(""),
  error = ref(""),
  connected = ref(false),
  connecting = ref(false);
const mac = ref(""),
  model = ref("tc1"),
  version = ref("v2"),
  password = ref(""),
  currentPassword = ref(""),
  newPassword = ref(""),
  importRef = ref(null),
  now = ref(Date.now());
const connection = reactive({
  host: `${location.protocol === "https:" ? "wss" : "ws"}://${location.host}/mqtt`,
  username: "",
  password: "",
});
const theme = ref(localStorage.getItem("ztc1_theme") || "auto");
watch(theme, value => {
  document.documentElement.classList.remove("light", "dark");
  if (["light", "dark"].includes(value)) document.documentElement.classList.add(value);
  localStorage.setItem("ztc1_theme", value);
}, { immediate: true });
function toggleTheme() { theme.value = { auto: "light", light: "dark", dark: "auto" }[theme.value] || "auto"; }
let client, timer;
const active = computed(() => {
  const d = list.value.find((d) => d.mac === selected.value);
  return d ? { ...states[d.mac], ...d } : null;
});
function disconnect() {
  if (client) {
    client.removeAllListeners();
    client.end(true);
    client = null;
  }
  connected.value = false;
  connecting.value = false;
  Object.keys(states).forEach((k) => delete states[k]);
}
function subscribe() {
  if (client && connected.value)
    for (const d of list.value.filter(
      (d) => !d.status || d.status === "active",
    ))
      client.subscribe(topics(d), { qos: 1 }, (e, grants) => {
        if (e || grants?.some((g) => g.qos === 128))
          error.value = "订阅被拒绝，请检查设备归属和 MQTT 账号";
      });
}
function connect() {
  disconnect();
  error.value = "";
  connecting.value = true;
  if (!/^wss?:\/\//.test(connection.host)) {
    connecting.value = false;
    error.value = "浏览器需要 ws:// 或 wss:// 地址";
    return;
  }
  client = mqtt.connect(connection.host, {
    username: account.username || connection.username,
    password: account.username ? password.value : connection.password,
    clientId: "web_" + crypto.randomUUID(),
    clean: true,
    connectTimeout: 5000,
    reconnectPeriod: 2000,
  });
  client.on("connect", () => {
    connected.value = true;
    connecting.value = false;
    subscribe();
  });
  client.on("close", () => {
    connected.value = false;
    for (const s of Object.values(states)) s.sensorFresh = false;
  });
  client.on("error", () => {
    error.value = "MQTT 连接失败，请检查地址、账号密码和权限";
    connecting.value = false;
  });
  client.on("message", (topic, payload) => {
    for (const d of list.value) {
      const s = (states[d.mac] ||= {});
      if (applyMessage(d, s, topic, payload.toString())) break;
    }
  });
  if (!account.username)
    localStorage.setItem("mqtt_config", JSON.stringify({ ...connection }));
}
function replaceList(value) {
  list.value = value.map(deviceEntry);
  if (!list.value.some((d) => d.mac === selected.value))
    selected.value = list.value[0]?.mac || "";
  subscribe();
}
async function refresh() {
  try {
    replaceList(account.username ? await api("/devices") : localDevices());
  } catch (e) {
    error.value = e.message;
  }
}
async function loadMode() {
  ready.value = false;
  disconnect();
  list.value = [];
  selected.value = "";
  password.value = account.password;
  error.value = "";
  connection.host = `${location.protocol === "https:" ? "wss" : "ws"}://${location.host}/mqtt`;
  connection.username = "";
  connection.password = "";
  try {
    if (account.username) {
      const s = await api("/register/status");
      if (s.mqtt_ws_uri) connection.host = s.mqtt_ws_uri;
    } else {
      const stored = JSON.parse(localStorage.getItem("mqtt_config") || "{}");
      Object.assign(connection, {
        host: stored.host || connection.host,
        username: stored.username || "",
        password: stored.password || "",
      });
    }
  } catch (e) {
    error.value = e.message;
  }
  await refresh();
  ready.value = true;
}
watch(
  () => account.username,
  () => {
    if (ready.value) loadMode();
  },
);
watch(
  list,
  (value) => {
    if (ready.value && !account.username) {
      localStorage.setItem("phicomm_devices", JSON.stringify(value));
      localStorage.setItem("ztc1_devices", JSON.stringify(value));
    }
  },
  { deep: true },
);
onMounted(async () => {
  await restoreAccount();
  await loadMode();
  timer = setInterval(() => (now.value = Date.now()), 1000);
});
onBeforeUnmount(() => {
  disconnect();
  clearInterval(timer);
});
async function add() {
  try {
    const d = deviceEntry({
      mac: mac.value,
      model: model.value,
      version: model.value === "tc1" ? version.value : "community",
    });
    if (account.username) {
      await api("/devices", { method: "POST", body: JSON.stringify(d) });
      await refresh();
    } else {
      if (list.value.some((x) => x.mac === d.mac))
        throw new Error("设备已存在");
      replaceList([...list.value, d]);
    }
    mac.value = "";
    selected.value = d.mac;
    error.value = "";
  } catch (e) {
    error.value = e.message;
    if (account.username) await refresh();
  }
}
async function remove(d) {
  if (
    !confirm(
      `解绑 ${d.mac}？${account.username ? "该账号的 MQTT 权限也将撤销。" : ""}`,
    )
  )
    return;
  try {
    if (account.username) {
      await api(`/devices/${d.mac}`, { method: "DELETE" });
      await refresh();
    } else replaceList(list.value.filter((x) => x.mac !== d.mac));
    if (client) client.unsubscribe(topics(d));
    delete states[d.mac];
  } catch (e) {
    error.value = e.message;
    if (account.username) await refresh();
  }
}
async function edit(d) {
  try {
    const value = prompt(
      "型号及协议：tc1/v1、tc1/v2、m1/community、a1/community",
      `${d.model}/${d.version}`,
    );
    if (!value) return;
    const [model, version] = value.split("/");
    const next = deviceEntry({ mac: d.mac, model, version });
    if (account.username)
      await api(`/devices/${d.mac}`, {
        method: "PUT",
        body: JSON.stringify(next),
      });
    else replaceList(list.value.map((x) => (x.mac === d.mac ? next : x)));
    if (client) client.unsubscribe(topics(d));
    delete states[d.mac];
    await refresh();
  } catch (e) {
    error.value = e.message;
    if (account.username) await refresh();
  }
}
function send(cmd) {
  if (!client || !connected.value) {
    error.value = "请先连接 MQTT";
    return;
  }
  const d = list.value.find((d) => d.mac === cmd.mac);
  if (!d || (d.status && d.status !== "active")) return;
  try {
    const c = command(d, cmd);
    client.publish(c.topic, c.payload, { qos: 1, retain: false }, (e) => {
      if (e) error.value = "命令发送失败";
    });
  } catch (e) {
    error.value = e.message;
  }
}
async function uploadLocal() {
  try {
    const entries = localDevices();
    if (!entries.length) throw new Error("本地没有设备");
    if (
      !confirm(
        `将 ${entries.length} 台本地设备绑定到当前账号？云端已有设备会保留。`,
      )
    )
      return;
    let count = 0;
    const failures = [];
    for (const d of entries) {
      const { status, ...entry } = d;
      try {
        await api("/devices", { method: "POST", body: JSON.stringify(entry) });
        count++;
      } catch (e) {
        failures.push(`${d.mac}: ${e.message}`);
      }
    }
    await refresh();
    error.value = `已导入 ${count} 台。${failures.join("；")}`;
  } catch (e) {
    error.value = e.message;
  }
}
async function logout() {
  try {
    await api("/auth/logout", { method: "POST", body: "{}" });
  } catch (e) {
    error.value = e.message;
  }
  account.username = null;
  account.password = "";
  disconnect();
}
async function changePassword() {
  try {
    await api("/auth/password", {
      method: "POST",
      body: JSON.stringify({
        current_password: currentPassword.value,
        password: newPassword.value,
      }),
    });
    account.username = null;
    account.password = "";
    currentPassword.value = "";
    newPassword.value = "";
    disconnect();
    error.value = "密码已修改，请重新登录，并更新设备端 MQTT 密码";
  } catch (e) {
    error.value = e.message;
  }
}
</script>
<template>
  <main class="console-page">
    <header>
      <h1>phicomm-community-mqtt</h1>
      <button @click="toggleTheme">主题：{{ theme === "auto" ? "自动" : theme === "dark" ? "深色" : "浅色" }}</button>
      <nav>
        <template v-if="account.username"
          >{{ account.username }} ·
          <button @click="logout">退出</button></template
        ><template v-else
          ><RouterLink to="/login">登录</RouterLink> ·
          <RouterLink to="/register">注册</RouterLink> · 访客模式</template
        >
        · <RouterLink to="/admin">管理</RouterLink>
      </nav>
    </header>
    <p role="alert" class="error">{{ error }}</p>
    <template v-if="ready"
      ><section class="card">
        <h2>
          MQTT {{ connected ? "已连接" : connecting ? "连接中" : "未连接" }}
        </h2>
        <form @submit.prevent="connect">
          <label
            >WebSocket 地址<input
              v-model="connection.host"
              :disabled="!!account.username || connected" /></label
          ><label
            >用户名<input
              :value="account.username || connection.username"
              @input="connection.username = $event.target.value"
              :disabled="!!account.username || connected"
              autocomplete="username" /></label
          ><label
            >密码<input
              v-if="account.username"
              v-model="password"
              type="password"
              autocomplete="current-password"
              :disabled="connected" /><input
              v-else
              v-model="connection.password"
              type="password"
              autocomplete="current-password"
              :disabled="connected" /></label
          ><button v-if="!connected" :disabled="connecting">连接</button
          ><button v-else type="button" @click="disconnect">断开</button>
        </form>
        <p>
          {{
            account.username
              ? "设备列表保存在云端，MQTT 密码仅保留在当前页面内存。"
              : "访客设备和连接配置仅保存在本浏览器。"
          }}
        </p>
      </section>
      <section class="card">
        <h2>{{ account.username ? "我的云端设备" : "本地设备" }}</h2>
        <button @click="refresh">刷新列表</button
        ><button v-if="account.username" @click="uploadLocal">
          导入本地设备到账号</button
        ><button v-else @click="importRef?.open()">导入 / 导出 / 扫码</button>
        <form @submit.prevent="add">
          <label
            >MAC<input
              v-model="mac"
              placeholder="AABBCCDDEEFF"
              required /></label
          ><label
            >型号<select v-model="model" aria-label="型号">
              <option value="tc1">tc1(a1)</option>
              <option>m1</option>
              <option>a1</option>
            </select></label
          ><label v-if="model === 'tc1'"
            >协议<select v-model="version" aria-label="协议">
              <option value="v1">v1（原固件）</option>
              <option value="v2">v2（社区固件）</option>
            </select></label
          ><button>添加设备</button>
        </form>
        <ul>
          <li v-for="d in list" :key="d.mac">
            <button
              @click="selected = d.mac"
              :aria-pressed="selected === d.mac"
            >
              {{ modelName(d.model) }} · {{ d.mac }} · {{ d.version }}
            </button>
            <span v-if="d.status && d.status !== 'active'">{{
              d.status === "deleting" ? "解绑待同步" : "权限待同步"
            }}</span>
            <button @click="edit(d)">修改型号 / 协议</button>
            <button @click="remove(d)">解绑</button>
          </li>
        </ul>
        <p v-if="!list.length">尚无设备</p>
      </section>
      <DeviceControl
        v-if="active?.model === 'tc1'"
        :key="active.mac + active.version + account.username"
        :device="active"
        :account-name="account.username || ''"
        @send-cmd="send" /><CommunityControl
        v-else-if="active"
        :key="active.mac"
        :device="active"
        :connected="connected"
        :now="now"
        @send-cmd="send" />
      <details v-if="account.username" class="card">
        <summary>修改账号密码</summary>
        <form @submit.prevent="changePassword">
          <label
            >当前密码<input
              v-model="currentPassword"
              type="password"
              autocomplete="current-password"
              required /></label
          ><label
            >新密码<input
              v-model="newPassword"
              type="password"
              minlength="6"
              autocomplete="new-password"
              required /></label
          ><button>修改并退出所有会话</button>
        </form>
      </details>
      <ImportExport
        v-if="!account.username"
        ref="importRef"
        @config-imported="loadMode"
    /></template>
  </main>
</template>
