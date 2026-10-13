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
import { useRoute, useRouter } from "vue-router";
import AppIcon from "./components/AppIcon.vue";
import ThemeToggle from "./components/ThemeToggle.vue";
const route = useRoute(), router = useRouter();
const guestMode = computed(() => route.name === "Guest");
const cloudUsername = computed(() => guestMode.value ? null : account.username);
import DeviceControl from "./components/DeviceControl.vue";
import CommunityControl from "./components/CommunityControl.vue";
import ImportExport from "./components/ImportExport.vue";
import { account, api, restoreAccount, refreshAccountRole } from "./account";
import {
  deviceKey,
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
let client, timer, cloudTimer, refreshing;
const active = computed(() => {
  const d = list.value.find((d) => deviceKey(d) === selected.value);
  return d ? { ...states[deviceKey(d)], ...d } : null;
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
    username: cloudUsername.value || connection.username,
    password: cloudUsername.value ? password.value : connection.password,
    clientId: "web_" + (crypto.randomUUID?.() || Array.from(crypto.getRandomValues(new Uint8Array(16)), b => b.toString(16).padStart(2, "0")).join("")),
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
      const s = (states[deviceKey(d)] ||= {});
      if (applyMessage(d, s, topic, payload.toString())) break;
    }
  });
  if (!cloudUsername.value)
    localStorage.setItem("mqtt_config", JSON.stringify({ ...connection }));
}
function replaceList(value) {
  const next = value.map(deviceEntry);
  if (JSON.stringify(next) === JSON.stringify(list.value)) return;
  if (client) for (const old of list.value) if (!next.some(d => deviceKey(d) === deviceKey(old))) client.unsubscribe(topics(old));
  list.value = next;
  if (!list.value.some((d) => deviceKey(d) === selected.value))
    selected.value = list.value[0] ? deviceKey(list.value[0]) : "";
  subscribe();
}
async function refresh() {
  const owner = cloudUsername.value;
  if (refreshing === owner) return; refreshing = owner;
  try {
    const value = owner ? await api("/devices") : localDevices();
    if (cloudUsername.value === owner) replaceList(value);
  } catch (e) {
    error.value = e.message;
  } finally { if (refreshing === owner) refreshing = undefined; }
}
async function loadMode() {
  ready.value = false;
  disconnect();
  list.value = [];
  selected.value = "";
  password.value = cloudUsername.value ? account.password : "";
  error.value = "";
  connection.host = `${location.protocol === "https:" ? "wss" : "ws"}://${location.host}/mqtt`;
  connection.username = "";
  connection.password = "";
  try {
    if (cloudUsername.value) {
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
  if (cloudUsername.value && password.value) connect();
}
watch(
  () => cloudUsername.value,
  () => {
    if (ready.value) { if (!guestMode.value && !cloudUsername.value) router.replace("/login"); else loadMode(); }
  },
);
watch(
  list,
  (value) => {
    if (ready.value && !cloudUsername.value) {
      localStorage.setItem("phicomm_devices", JSON.stringify(value));
      localStorage.setItem("ztc1_devices", JSON.stringify(value));
    }
  },
  { deep: true },
);
onMounted(async () => {
  if (!guestMode.value) await restoreAccount();
  await loadMode();
  timer = setInterval(() => (now.value = Date.now()), 1000);
  cloudTimer = setInterval(() => { if (ready.value && cloudUsername.value && !document.hidden) { refresh(); refreshAccountRole().catch(() => {}); } }, 3000);
});
onBeforeUnmount(() => {
  disconnect();
  clearInterval(timer);
  clearInterval(cloudTimer);
});
async function restoreScoped() {
  try {
    const value = prompt("输入需要恢复的账号主题设备 MAC（恢复后固件可重新控制）");
    if (!value) return;
    const d = deviceEntry({ mac: value });
    await api(`/devices/scoped/${d.mac}`, { method: "PUT", body: "{}" });
    await refresh();
  } catch (e) { error.value = e.message; }
}
async function add() {
  try {
    const d = deviceEntry({
      mac: mac.value,
      model: model.value,
      version: model.value === "tc1" ? version.value : "community",
    });
    if (cloudUsername.value) {
      await api("/devices", { method: "POST", body: JSON.stringify(d) });
      await refresh();
    } else {
      if (list.value.some((x) => x.mac === d.mac))
        throw new Error("设备已存在");
      replaceList([...list.value, d]);
    }
    mac.value = "";
    selected.value = deviceKey(d);
    error.value = "";
  } catch (e) {
    error.value = e.message;
    if (cloudUsername.value) await refresh();
  }
}
async function remove(d) {
  if (
    !confirm(
      `解绑 ${d.mac}？${cloudUsername.value ? "该设备的控制权限将撤销；账号主题的发现声明会被忽略。" : ""}`,
    )
  )
    return;
  try {
    if (cloudUsername.value) {
      await api(`/devices/${d.namespace === "account" ? "scoped/" : ""}${d.mac}`, { method: "DELETE" });
      await refresh();
    } else replaceList(list.value.filter((x) => deviceKey(x) !== deviceKey(d)));
    if (client) client.unsubscribe(topics(d));
    delete states[deviceKey(d)];
  } catch (e) {
    error.value = e.message;
    if (cloudUsername.value) await refresh();
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
    if (cloudUsername.value)
      await api(`/devices/${d.namespace === "account" ? "scoped/" : ""}${d.mac}`, {
        method: "PUT",
        body: JSON.stringify(next),
      });
    else replaceList(list.value.map((x) => (x.mac === d.mac ? next : x)));
    if (client) client.unsubscribe(topics(d));
    delete states[deviceKey(d)];
    await refresh();
  } catch (e) {
    error.value = e.message;
    if (cloudUsername.value) await refresh();
  }
}
function send(cmd) {
  if (!client || !connected.value) {
    error.value = "请先连接 MQTT";
    return;
  }
  const d = list.value.find((d) => deviceKey(d) === selected.value && d.mac === cmd.mac);
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
  account.isAdmin = false;
  account.password = "";
  disconnect();
  await router.replace("/login");
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
    account.isAdmin = false;
    account.password = "";
    currentPassword.value = "";
    newPassword.value = "";
    disconnect();
    await router.replace("/login");
  } catch (e) {
    error.value = e.message;
  }
}
</script>
<template>
 <main class="console-page">
  <header class="console-topbar">
   <RouterLink :to="cloudUsername ? '/' : '/guest'" class="brand"><span class="brand-mark"><AppIcon name="plug" :size="23" /></span><span>Phicomm<span class="brand-subtitle">COMMUNITY MQTT</span></span></RouterLink>
   <nav class="console-nav"><span class="mode-badge"><AppIcon :name="cloudUsername ? 'cloud' : 'guest'" :size="15" />{{ cloudUsername ? '账号模式' : '访客模式' }}</span><ThemeToggle /><RouterLink v-if="cloudUsername && account.isAdmin" to="/admin" class="nav-link"><AppIcon name="settings" :size="17" />管理</RouterLink><span v-if="cloudUsername" class="account-name">{{ cloudUsername }}</span><button v-if="cloudUsername" class="quiet-button" @click="logout"><AppIcon name="logout" :size="17" />退出</button><RouterLink v-else to="/login" class="secondary-button">登录</RouterLink></nav>
  </header>
  <div class="page-heading"><div><span class="eyebrow">{{ cloudUsername ? '我的智能空间' : '连接自己的设备' }}</span><h1>设备控制台</h1><p>{{ cloudUsername ? '设备状态与日常控制，集中在这里。' : '使用自己的 MQTT 服务器，设备列表保存在本浏览器。' }}</p></div><span class="device-count"><b>{{ list.length }}</b> 台设备</span></div>
  <p v-if="error" role="alert" class="feedback error">{{ error }}</p><p v-else role="alert" class="sr-only"></p>
  <template v-if="ready">
   <section class="surface connection-panel">
    <div class="section-heading"><div class="connection-title"><span class="connection-icon"><AppIcon name="signal" :size="22" /></span><div><h2>MQTT {{ connected ? '已连接' : connecting ? '连接中' : '未连接' }}</h2><p>{{ cloudUsername ? '使用平台账号连接设备' : '连接你的 MQTT WebSocket 服务' }}</p></div></div><span class="connection-actions"><span class="status-pill" :class="{ online: connected }"><span class="status-dot" :class="{ online: connected }"></span>{{ connected ? '连接正常' : connecting ? '正在连接' : '等待连接' }}</span><button v-if="connected && !cloudUsername" class="quiet-button" type="button" @click="disconnect">断开</button></span></div>
    <details class="connection-settings" :open="!cloudUsername && !connected"><summary><AppIcon name="settings" :size="16" />连接设置<span>{{ connected ? connection.host : '服务器、用户名与密码' }}</span></summary>
     <div v-if="cloudUsername" class="account-connection-settings"><p>服务器：{{ connection.host }}</p><p>账号：{{ cloudUsername }}</p><p class="helper-text">登录后自动连接，网络中断后自动重连。</p><button v-if="password" class="secondary-button" @click="connect">重新连接</button><RouterLink v-else to="/login" class="secondary-button">重新登录以恢复连接</RouterLink><button v-if="connected" class="quiet-button" @click="disconnect">断开</button></div>
     <form v-else class="connection-form" @submit.prevent="connect">
      <label class="server-field">WebSocket 地址<input v-model="connection.host" :disabled="!!cloudUsername || connected" placeholder="wss://mqtt.example.com/mqtt" spellcheck="false" /></label>
      <label>用户名<input :value="cloudUsername || connection.username" @input="connection.username = $event.target.value" :disabled="!!cloudUsername || connected" autocomplete="username" placeholder="MQTT 用户名（可选）" /></label>
      <label>密码<input v-model="connection.password" type="password" autocomplete="current-password" :disabled="connected" placeholder="MQTT 密码（可选）" /></label>
      <button v-if="!connected" class="primary-button" :disabled="connecting"><AppIcon name="signal" :size="17" />{{ connecting ? '连接中…' : '连接' }}</button>
     </form>
     <p class="helper-text">{{ cloudUsername ? '账号主题设备自动同步到云端；MQTT 密码仅保留在页面内存。' : '浏览器使用 ws:// 或 wss:// 地址连接，可填写任意支持 WebSocket 的 MQTT 服务器。' }}</p>
    </details>
   </section>
   <div class="dashboard-grid">
    <aside class="surface devices-panel">
     <div class="section-heading"><h2>{{ cloudUsername ? '我的云端设备' : '本地设备' }}</h2><button class="icon-button" @click="refresh" aria-label="刷新列表" title="刷新列表"><AppIcon name="signal" :size="18" /></button></div>
     <ul class="device-list">
      <li v-for="d in list" :key="deviceKey(d)" class="device-row" :class="{ selected: selected === deviceKey(d) }">
       <button class="device-select" @click="selected = deviceKey(d)" :aria-pressed="selected === deviceKey(d)" :aria-label="`${modelName(d.model)} · ${d.mac} · ${d.version}`"><span class="model-icon" :class="`model-${d.model}`"><AppIcon :name="d.model === 'tc1' ? 'plug' : d.model === 'm1' ? 'air' : 'fan'" :size="23" /></span><span class="device-label"><b>{{ modelName(d.model) }}</b><small class="device-mac">{{ d.mac }}</small><small class="device-origin">{{ d.namespace ? '账号主题 · 自动发现' : d.version === 'v1' ? '原固件 · v1' : '手动添加' }}</small></span></button>
       <div class="device-row-actions"><button v-if="!d.namespace" class="icon-button" @click="edit(d)" aria-label="修改型号 / 协议" title="修改型号 / 协议"><AppIcon name="edit" :size="15" /></button><button class="icon-button danger-text" @click="remove(d)" aria-label="解绑" title="解绑"><AppIcon name="close" :size="16" /></button></div>
       <small v-if="d.status && d.status !== 'active'" class="sync-status">{{ d.status === 'deleting' ? '解绑待同步' : '权限待同步' }}</small>
      </li>
     </ul>
     <div v-if="!list.length" class="device-list-empty"><AppIcon name="plug" :size="28" /><p>尚无设备</p><small>{{ cloudUsername ? '新固件上线后会自动出现' : '添加设备后即可查看状态' }}</small></div>
     <details class="add-device" open><summary><AppIcon name="plus" :size="17" />手动添加设备</summary><form class="add-device-form" @submit.prevent="add"><label class="full-width">MAC<input v-model="mac" placeholder="AABBCCDDEEFF" spellcheck="false" required /></label><label>型号<select v-model="model" aria-label="型号"><option value="tc1">tc1(a1)</option><option>m1</option><option>a1</option></select></label><label v-if="model === 'tc1'">协议<select v-model="version" aria-label="协议"><option value="v1">v1（原固件）</option><option value="v2">v2（社区固件）</option></select></label><button class="secondary-button full-width"><AppIcon name="plus" :size="17" />添加设备</button></form></details>
     <div class="device-tools"><button v-if="cloudUsername" class="text-button" @click="uploadLocal">导入本地设备到账号</button><button v-if="cloudUsername" class="text-button" @click="restoreScoped">恢复已解绑的账号主题设备</button><button v-else class="text-button" @click="importRef?.open()">导入 / 导出 / 扫码</button></div>
    </aside>
    <div class="device-area">
     <template v-if="active"><div class="device-area-heading"><span><span class="eyebrow">设备控制</span><h2>{{ modelName(active.model) }}</h2></span><span class="status-pill" :class="{ online: connected && active.available !== false }"><span class="status-dot" :class="{ online: connected && active.available !== false }"></span>{{ !connected ? '未连接' : active.available === false ? '离线' : '已连接' }}</span></div><DeviceControl v-if="active.model === 'tc1'" :key="deviceKey(active) + active.version" :device="active" :account-name="cloudUsername || ''" @send-cmd="send" /><CommunityControl v-else :key="deviceKey(active)" :device="active" :connected="connected" :now="now" @send-cmd="send" /></template>
     <section v-else class="surface empty-control"><span class="empty-control-icon"><AppIcon name="plug" :size="38" /></span><h2>从一台设备开始</h2><p>{{ cloudUsername ? '等待设备自动同步，或在左侧手动添加。' : '填写 MQTT 服务器，再添加你想控制的设备。' }}</p><span class="supported-models">tc1(a1)<span>·</span>m1<span>·</span>a1</span></section>
     <details v-if="cloudUsername" class="surface password-settings"><summary><AppIcon name="shield" :size="17" />账号安全<span>修改密码</span></summary><form @submit.prevent="changePassword"><label>当前密码<input v-model="currentPassword" type="password" autocomplete="current-password" required /></label><label>新密码<input v-model="newPassword" type="password" minlength="6" autocomplete="new-password" required /></label><button class="secondary-button">修改并退出所有会话</button></form></details>
    </div>
   </div>
   <ImportExport v-if="!cloudUsername" ref="importRef" @config-imported="loadMode" />
  </template>
  <div v-else class="surface loading-state">正在加载设备…</div>
  <footer class="console-footer">phicomm-community-mqtt<span>{{ cloudUsername ? '云端设备列表 · 私有控制权限' : '访客模式 · 本地设备列表' }}</span></footer>
 </main>
</template>
