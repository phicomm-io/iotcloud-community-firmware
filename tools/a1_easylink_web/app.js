"use strict";

const byId = (id) => document.getElementById(id);
const token = document.querySelector('meta[name="a1-token"]').content;
let busy = false;
let online = false;
let posting = false;
let polling = false;

function controls() {
  byId("inputs").disabled = busy || posting;
  byId("start").disabled = busy || posting || !online;
  byId("stop").disabled = !busy || posting || !online;
}

function render(state) {
  online = true;
  busy = state.phase === "sending";
  byId("message").textContent = state.message;
  byId("progress").max = state.duration;
  byId("progress").value = Math.min(state.elapsed, state.duration);
  byId("counts").textContent = `用时 ${state.elapsed} 秒 · 已发送 ${state.count} 包 · 完整轮次 ${state.cycles}`;
  controls();
}

async function request(path, body) {
  const options = {cache: "no-store"};
  if (body !== undefined) {
    options.method = "POST";
    options.headers = {"Content-Type": "application/json", "X-A1-Token": token};
    options.body = JSON.stringify(body);
  }
  const response = await fetch(path, options);
  const data = await response.json();
  if (!response.ok) throw new Error(data.error || "本机服务返回错误");
  return data;
}

async function poll() {
  if (posting || polling) return;
  polling = true;
  try {
    const state = await request("/api/status");
    if (!posting) render(state);
  } catch (_) {
    online = false;
    byId("message").textContent = "无法连接本机服务，请确认终端中的工具仍在运行。";
    controls();
  } finally {
    polling = false;
  }
}

byId("open-network").addEventListener("change", (event) => {
  const open = event.target.checked;
  byId("password").disabled = open;
  byId("password").required = !open;
  if (open) byId("password").value = "";
});

byId("provision").addEventListener("submit", async (event) => {
  event.preventDefault();
  if (posting || busy || !online) return;
  posting = true;
  controls();
  try {
    const state = await request("/api/start", {
      ssid: byId("ssid").value,
      password: byId("password").value,
      open_network: byId("open-network").checked,
      interface_ip: byId("interface-ip").value.trim(),
      duration: Number(byId("duration").value),
      interval_ms: Number(byId("interval").value),
    });
    byId("password").value = "";
    render(state);
  } catch (error) {
    byId("message").textContent = error.message;
  } finally {
    posting = false;
    controls();
  }
});

byId("stop").addEventListener("click", async () => {
  posting = true;
  controls();
  try {
    render(await request("/api/stop", {}));
  } catch (error) {
    byId("message").textContent = error.message;
  } finally {
    posting = false;
    controls();
  }
});

poll();
setInterval(poll, 750);
