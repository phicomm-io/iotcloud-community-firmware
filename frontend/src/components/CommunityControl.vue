<script setup>
import { ref, watch } from "vue";
const props = defineProps({
  device: { type: Object, required: true },
  connected: Boolean,
  now: Number,
});
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
  <section class="card">
    <h2>
      {{ device.deviceName || device.model }}
      <small>{{
        device.available === undefined
          ? "状态未知"
          : device.available
            ? "在线"
            : "离线"
      }}</small>
    </h2>
    <template v-if="device.model === 'm1'"
      ><p
        v-if="
          !connected ||
          !device.available ||
          !device.sensorFresh ||
          !device.sensorAt ||
          now - device.sensorAt > 15000
        "
      >
        传感器数据无效或已过期
      </p>
      <dl v-else>
        <template
          v-for="[key, title, unit] in [
            ['temperature', '温度', '°C'],
            ['humidity', '湿度', '%'],
            ['formaldehyde', '甲醛', 'mg/m³'],
            ['pm25', 'PM2.5', 'µg/m³'],
          ]"
          :key="key"
          ><dt>{{ title }}</dt>
          <dd>{{ device[key] ?? "—" }} {{ unit }}</dd></template
        >
      </dl>
      <form @submit.prevent="send('brightness/set', Number(brightness))">
        <label
          >屏幕亮度（设备请求值）<select
            v-model="brightness"
            aria-label="屏幕亮度（设备请求值）"
          >
            <option v-for="n in 5" :value="n - 1" :key="n">{{ n - 1 }}</option>
          </select></label
        ><button :disabled="!connected">设置亮度</button>
      </form></template
    >
    <template v-else
      ><p>
        风扇：{{ device.fan ?? "未知" }} · 转速：{{
          device.fan_percentage ?? "—"
        }}%
      </p>
      <button
        :disabled="!connected"
        @click="send('fan/set', device.fan === 'ON' ? 'OFF' : 'ON')"
      >
        切换风扇
      </button>
      <form @submit.prevent="send('fan/percentage/set', Number(percentage))">
        <label
          >风扇转速<input
            v-model="percentage"
            type="number"
            min="0"
            max="100"
            required /></label
        ><button :disabled="!connected">设置转速</button>
      </form>
      <p>灯光：{{ device.light?.state ?? "未知" }}</p>
      <label>颜色<input v-model="rgb" type="color" /></label
      ><label
        >亮度<input
          v-model="lightBrightness"
          type="range"
          min="0"
          max="255" /></label
      ><button :disabled="!connected" @click="light('ON')">开启灯光</button
      ><button :disabled="!connected" @click="light('OFF')">关闭灯光</button>
      <p
        v-for="[key, title] in [
          ['child_lock', '童锁'],
          ['led_always_on', '指示灯常亮'],
        ]"
        :key="key"
      >
        {{ title }}：{{ device[key] ?? "未知" }}
        <button
          :disabled="!connected"
          @click="send(key + '/set', device[key] === 'ON' ? 'OFF' : 'ON')"
        >
          切换
        </button>
      </p></template
    >
  </section>
</template>
