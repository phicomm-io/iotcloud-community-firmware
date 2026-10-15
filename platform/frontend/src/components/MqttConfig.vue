<script setup>
import { ref, reactive, computed, onMounted } from 'vue'

const props = defineProps({
  connected: Boolean
})

const emit = defineEmits(['connect', 'disconnect'])

const config = reactive({
  host: 'ws://localhost:8083/mqtt',
  username: '',
  password: '',
  clientId: 'web_client_' + Math.random().toString(16).substr(2, 8)
})

const isExpanded = ref(true)

const defaultHost = computed(() => {
  const proto = window.location.protocol === 'https:' ? 'wss' : 'ws'
  return `${proto}://${window.location.hostname}/mqtt`
})

onMounted(() => {
  const saved = localStorage.getItem('mqtt_config')
  if (saved) {
    const savedConfig = JSON.parse(saved)
    // Don't restore clientId to avoid conflicts when importing config to other devices
    // We always want a fresh random ClientID for a new session/device
    delete savedConfig.clientId 
    Object.assign(config, savedConfig)
    
    // Attempt auto connect if configured
    if (localStorage.getItem('mqtt_auto_connect') === 'true') {
      handleConnect()
    }
  }
})

const handleConnect = () => {
  localStorage.setItem('mqtt_config', JSON.stringify(config))
  localStorage.setItem('mqtt_auto_connect', 'true')
  emit('connect', { ...config })
  isExpanded.value = false
}

const handleDisconnect = () => {
  localStorage.setItem('mqtt_auto_connect', 'false')
  emit('disconnect')
}

const toggleExpand = () => {
  isExpanded.value = !isExpanded.value
}
</script>

<template>
  <div class="mqtt-config card">
    <div class="header" @click="toggleExpand">
      <h3>
        MQTT 设置 
        <span class="status-indicator" :class="{ connected: connected }"></span>
        {{ connected ? '已连接' : '未连接' }}
      </h3>
      <button class="toggle-btn">{{ isExpanded ? '▼' : '▶' }}</button>
    </div>
    
    <div v-if="isExpanded" class="form-body">
      <div class="form-group">
        <label>服务器地址 ({{ defaultHost }})</label>
        <input v-model="config.host" :placeholder="defaultHost" :disabled="connected">
      </div>
      <div class="form-group">
        <label>用户名</label>
        <input v-model="config.username" placeholder="User" :disabled="connected">
      </div>
      <div class="form-group">
        <label>密码</label>
        <input v-model="config.password" type="password" placeholder="Password" :disabled="connected">
      </div>
       <div class="actions">
        <button v-if="!connected" @click="handleConnect" class="primary-btn">连接</button>
        <button v-else @click="handleDisconnect" class="danger-btn">断开</button>
      </div>
    </div>
  </div>
</template>

<style scoped>
.mqtt-config {
  margin-bottom: 20px;
}
.header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  cursor: pointer;
  padding: 10px;
  background: var(--bg-secondary);
  border-radius: 4px;
  color: var(--text-primary);
}
.status-indicator {
  display: inline-block;
  width: 10px;
  height: 10px;
  border-radius: 50%;
  background: #ccc;
  margin-left: 10px;
}
.status-indicator.connected {
  background: #4caf50;
}
.form-body {
  padding: 15px;
  border: 1px solid var(--border-color);
  border-top: none;
  background: var(--bg-card);
}
.form-group {
  margin-bottom: 15px;
}
.form-group label {
  display: block;
  margin-bottom: 5px;
  color: var(--text-secondary);
}
.form-group input {
  width: 100%;
  padding: 8px;
  box-sizing: border-box;
  background: var(--bg-input);
  color: var(--text-primary);
  border: 1px solid var(--border-color);
  border-radius: 4px;
}
.actions {
  text-align: right;
}
button {
  padding: 8px 16px;
  cursor: pointer;
  border-radius: 4px;
}
.primary-btn {
  background: #4caf50;
  color: white;
  border: none;
}
.danger-btn {
  background: #f44336;
  color: white;
  border: none;
}
.toggle-btn {
  background: none;
  border: none;
  font-size: 12px;
  color: var(--text-primary);
}
</style>
