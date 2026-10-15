<script setup>
import { ref, watch, computed } from "vue";
import AppIcon from "./AppIcon.vue";
const props = defineProps({
  device: { type: Object, required: true },
  connected: Boolean,
  now: Number,
});
const sensorValid = computed(() => props.connected && props.device.available && props.device.sensorFresh && props.device.sensorAt && props.now - props.device.sensorAt <= 15000);
const emit = defineEmits(["send-cmd"]);
const brightness = ref(0),
  percentage = ref(0),
  rgb = ref("#ffffff"),
  lightBrightness = ref(255);
watch(
  () => props.device.brightness,
  (v) => {
    if (v !== undefined) brightness.value = Number(v);
  },
  { immediate: true },
);
watch(
  () => props.device.fan_percentage,
  (v) => {
    if (v !== undefined) percentage.value = Number(v);
  },
  { immediate: true },
);
watch(
  () => props.device.light,
  (v) => {
    if (!v) return;
    lightBrightness.value = v.brightness ?? 255;
    if (v.color)
      rgb.value =
        "#" +
        ["r", "g", "b"]
          .map((k) => Number(v.color[k]).toString(16).padStart(2, "0"))
          .join("");
  },
  { immediate: true },
);
const send = (leaf, value) =>
  emit("send-cmd", { mac: props.device.mac, leaf, value });
function light(state) {
  const c = rgb.value;
  send("light/set", {
    state,
    brightness: Number(lightBrightness.value),
    color: {
      r: parseInt(c.slice(1, 3), 16),
      g: parseInt(c.slice(3, 5), 16),
      b: parseInt(c.slice(5, 7), 16),
    },
  });
}
</script>
<template>
 <section class="card community-control">
  <div class="community-heading"><span class="model-icon" :class="`model-${device.model}`"><AppIcon :name="device.model === 'm1' ? 'air' : 'fan'" :size="25" /></span><div><h2>{{ device.deviceName || device.model }}</h2><p>{{ device.model === 'm1' ? '空气质量检测' : '空气净化与灯光' }}</p></div><span class="status-pill" :class="{ online: device.available && connected }"><span class="status-dot" :class="{ online: device.available && connected }"></span>{{ !connected || device.available === undefined ? '状态未知' : device.available ? '在线' : '离线' }}</span></div>
  <template v-if="device.model === 'm1'">
   <div v-if="!sensorValid" class="sensor-warning"><AppIcon name="air" :size="21" /><p>传感器数据无效或已过期</p></div>
   <dl v-else class="sensor-grid"><div v-for="[key,title,unit] in [['temperature','温度','°C'],['humidity','湿度','%'],['formaldehyde','甲醛','mg/m³'],['pm25','PM2.5','µg/m³']]" :key="key" class="sensor-metric"><dt>{{ title }}</dt><dd>{{ device[key] ?? '—' }} <span>{{ unit }}</span></dd></div></dl>
   <div class="control-divider"></div>
   <form class="brightness-control" @submit.prevent="send('brightness/set',Number(brightness))"><span class="control-section-icon"><AppIcon name="monitor" :size="23" /></span><label>屏幕亮度（设备请求值）<select v-model="brightness" aria-label="屏幕亮度（设备请求值）"><option v-for="n in 5" :value="n-1" :key="n">{{ n-1 }}</option></select></label><button class="secondary-button" :disabled="!connected">设置亮度</button></form>
  </template>
  <template v-else>
   <div class="a1-module"><div class="a1-module-heading"><span class="control-section-icon"><AppIcon name="fan" :size="23" /></span><div><h3>风扇</h3><p>风扇：{{ device.fan ?? '未知' }} · 转速：{{ device.fan_percentage ?? '—' }}%</p></div><button class="secondary-button" :disabled="!connected" @click="send('fan/set',device.fan === 'ON' ? 'OFF' : 'ON')">切换风扇</button></div><form class="fan-speed-control" @submit.prevent="send('fan/percentage/set',Number(percentage))"><label>风扇转速<span class="percentage-field"><input v-model="percentage" aria-label="风扇转速" type="number" min="0" max="100" required /><span>%</span></span></label><button class="secondary-button" :disabled="!connected">设置转速</button></form></div>
   <div class="control-divider"></div>
   <div class="a1-module"><div class="a1-module-heading"><span class="control-section-icon"><AppIcon name="sun" :size="23" /></span><div><h3>RGB 灯光</h3><p>灯光：{{ device.light?.state ?? '未知' }}</p></div></div><div class="light-inputs"><label>颜色<input v-model="rgb" type="color" /></label><label class="light-range">亮度 <span class="range-value">{{ lightBrightness }} / 255</span><input v-model="lightBrightness" type="range" min="0" max="255" /></label></div><div class="light-actions"><button class="secondary-button" :disabled="!connected" @click="light('ON')">开启灯光</button><button class="quiet-button" :disabled="!connected" @click="light('OFF')">关闭灯光</button></div></div>
   <div class="control-divider"></div>
   <div class="extra-controls"><div v-for="[key,title] in [['child_lock','童锁'],['led_always_on','指示灯常亮']]" :key="key" class="extra-control"><span><AppIcon :name="key === 'child_lock' ? 'shield' : 'sun'" :size="18" />{{ title }}<small>{{ device[key] ?? '未知' }}</small></span><button class="toggle-control" :class="{ on: device[key] === 'ON' }" :disabled="!connected" :aria-label="`切换${title}`" :aria-pressed="device[key] === 'ON'" @click="send(key+'/set',device[key] === 'ON' ? 'OFF' : 'ON')"><span></span></button></div></div>
  </template>
 </section>
</template>
<style scoped>
.community-control { padding:26px; }
.community-heading { display:flex; align-items:center; gap:13px; padding-bottom:22px; border-bottom:1px solid var(--border-light); }
.community-heading > div { flex:1; min-width:0; }.community-heading h2 { font-size:17px; }.community-heading p { font-size:11px; margin-top:4px; }.community-heading .status-pill { font-size:11px; }
.sensor-grid { display:grid; grid-template-columns:repeat(2,minmax(0,1fr)); gap:15px; padding:0; margin:24px 0; }
.sensor-metric { padding:20px; border-radius:12px; background:var(--bg-secondary); }.sensor-metric dt { font-size:12px; color:var(--text-secondary); }.sensor-metric dd { font-size:30px; letter-spacing:-1px; font-weight:600; line-height:1.3; margin:12px 0 0; white-space:nowrap; }.sensor-metric dd span { font-size:12px; letter-spacing:0; font-weight:400; color:var(--text-secondary); }
.sensor-warning { display:flex; align-items:center; justify-content:center; flex-direction:column; gap:12px; color:var(--text-muted); padding:60px 15px; background:var(--bg-secondary); border-radius:12px; margin:24px 0; }.sensor-warning p { font-size:13px; text-align:center; }
.control-divider { border-top:1px solid var(--border-light); margin:23px 0; }
.control-section-icon { width:42px; height:42px; display:grid; place-items:center; border-radius:12px; background:var(--bg-secondary); color:var(--text-secondary); flex-shrink:0; }
.brightness-control { display:flex; align-items:end; gap:15px; }.brightness-control .control-section-icon { align-self:center; }.brightness-control label { flex:1; }.brightness-control select { width:120px; }.brightness-control button { min-height:43px; }
.a1-module-heading { display:flex; gap:13px; align-items:center; }.a1-module-heading > div { flex:1; }.a1-module-heading h3 { font-size:14px; margin-bottom:5px; }.a1-module-heading p { font-size:12px; }.a1-module { margin-top:23px; }
.fan-speed-control { display:flex; align-items:end; gap:14px; margin:22px 0 0 55px; }.fan-speed-control label { flex:1; max-width:220px; }.fan-speed-control button { min-height:43px; }.percentage-field { position:relative; }.percentage-field > span { position:absolute; top:10px; right:14px; color:var(--text-muted); }.percentage-field input { padding-right:33px; }
.light-inputs { display:flex; gap:24px; align-items:center; margin:20px 0 0 55px; }.light-inputs label:first-child { width:72px; }.light-range { flex:1; position:relative; }.range-value { position:absolute; top:0; right:0; font-size:11px; color:var(--text-muted); }.light-actions { display:flex; gap:12px; margin:15px 0 0 55px; }
.extra-controls { display:flex; flex-direction:column; gap:19px; }.extra-control { display:flex; align-items:center; justify-content:space-between; gap:12px; }.extra-control > span { display:flex; align-items:center; gap:10px; font-size:13px; }.extra-control small { color:var(--text-muted); margin-left:8px; font-size:11px; }.toggle-control { min-height:25px; height:25px; width:45px; padding:3px; background:var(--switch-bg); border:0; border-radius:20px; display:flex; justify-content:start; }.toggle-control span { width:19px; height:19px; border-radius:50%; background:var(--bg-card); box-shadow:0 1px 4px var(--shadow-color); }.toggle-control.on { background:var(--accent); justify-content:end; }.toggle-control.on:hover { background:var(--accent-hover); }
@media(max-width:520px) { .community-control { padding:19px; }.community-heading { gap:10px; }.community-heading h2 { font-size:15px; }.community-heading .model-icon { width:39px; height:42px; }.sensor-grid { gap:10px; }.sensor-metric { padding:17px 13px; }.sensor-metric dd { font-size:25px; }.sensor-metric dd span { font-size:10px; }.brightness-control { gap:10px; flex-wrap:wrap; }.brightness-control .control-section-icon { display:none; }.brightness-control label { min-width:130px; }.brightness-control select { width:100%; }.fan-speed-control,.light-inputs,.light-actions { margin-left:0; }.a1-module-heading { gap:10px; }.a1-module-heading button { font-size:11px; padding:8px 10px; }.a1-module-heading p { font-size:11px; }.fan-speed-control label { max-width:none; }.light-inputs { gap:18px; } }
</style>
