<script setup>
import { reactive, watch, ref } from 'vue'

const props = defineProps({
  task: Object,
  index: Number
})

const emit = defineEmits(['save'])

const editing = ref(false)
const localTask = reactive({
  hour: 0,
  minute: 0,
  repeat: 0,
  action: 1,
  on: 0
})

const days = ['一', '二', '三', '四', '五', '六', '日']

// Init local state when editing starts or props change
watch(() => props.task, (val) => {
  if (val) {
    Object.assign(localTask, val)
  }
}, { immediate: true })

const toggleDay = (dayIndex) => {
  // Flip bit at dayIndex
  localTask.repeat ^= (1 << dayIndex)
}

const isDaySelect = (dayIndex) => {
  return (localTask.repeat >> dayIndex) & 1
}

const save = () => {
  emit('save', { ...localTask })
  editing.value = false
}

const startEdit = () => {
    // If no task exists yet, init default
    if (!props.task) {
        Object.assign(localTask, {
            hour: 12, minute: 0, repeat: 0, action: 1, on: 1
        })
    } else {
        Object.assign(localTask, props.task)
    }
    editing.value = true
}

const cancel = () => {
    editing.value = false
}

const formatRepeat = (bits) => {
  if (bits === 0) return '一次'
  if (bits === 127) return '每天'
  let res = []
  for(let i=0; i<7; i++) {
    if ((bits >> i) & 1) res.push(days[i])
  }
  return res.length > 0 ? '周' + res.join('') : '一次'
}

</script>

<template>
  <div class="task-editor">
    <div v-if="!editing" class="task-summary" @click="startEdit">
      <div v-if="task && task.on" class="active-task">
        <span class="time">{{ String(task.hour).padStart(2,'0') }}:{{ String(task.minute).padStart(2,'0') }}</span>
        <span class="action" :class="task.action ? 'on' : 'off'">{{ task.action ? '开' : '关' }}</span>
        <span class="repeat">{{ formatRepeat(task.repeat) }}</span>
      </div>
      <div v-else class="empty-task">
        任务 {{ index + 1 }} (未启用)
      </div>
    </div>

    <div v-else class="edit-form">
      <div class="time-picker">
        <input type="number" v-model="localTask.hour" min="0" max="23"> :
        <input type="number" v-model="localTask.minute" min="0" max="59">
      </div>
      
      <div class="days-picker">
        <button 
            v-for="(d, i) in days" 
            :key="i"
            @click="toggleDay(i)"
            :class="{ selected: isDaySelect(i) }"
        >{{ d }}</button>
      </div>

      <div class="options">
        <label>
            动作: 
            <select v-model="localTask.action">
                <option :value="1">开启</option>
                <option :value="0">关闭</option>
            </select>
        </label>
        <label>
            启用: 
            <input type="checkbox" v-model="localTask.on" :true-value="1" :false-value="0">
        </label>
      </div>

      <div class="actions">
        <button @click="save" class="save-btn">保存</button>
        <button @click="cancel" class="cancel-btn">取消</button>
      </div>
    </div>
  </div>
</template>

<style scoped>
.task-editor {
  border-top: 1px solid var(--border-light);
  padding: 8px 0;
}
.task-summary {
  cursor: pointer;
  padding: 5px;
  background: var(--bg-secondary);
  border-radius: 4px;
}
.task-summary:hover {
    background: var(--bg-hover);
}
.active-task {
    display: flex;
    gap: 10px;
    align-items: center;
    font-size: 0.9em;
}
.time { font-weight: bold; }
.action.on { color: green; }
.action.off { color: red; }
.repeat { color: var(--text-muted); font-size: 0.8em; }

.empty-task { color: var(--text-muted); font-size: 0.8em; }

.edit-form {
    padding: 10px;
    background: var(--bg-card);
    border: 1px solid var(--border-light);
    margin-top: 5px;
}
.time-picker input {
    width: 40px;
    text-align: center;
    background: var(--bg-input);
    color: var(--text-primary);
    border: 1px solid var(--border-color);
    border-radius: 4px;
}
.days-picker {
    display: flex;
    gap: 2px;
    margin: 10px 0;
}
.days-picker button {
    flex: 1;
    padding: 4px 0;
    font-size: 10px;
    border: 1px solid var(--border-color);
    background: var(--bg-card);
    color: var(--text-primary);
    cursor: pointer;
}
.days-picker button.selected {
    background: #2196f3;
    color: white;
    border-color: #1976d2;
}
.options {
    display: flex;
    justify-content: space-between;
    margin-bottom: 10px;
    font-size: 0.9em;
    color: var(--text-primary);
}
.options select {
    background: var(--bg-input);
    color: var(--text-primary);
    border: 1px solid var(--border-color);
    border-radius: 4px;
}
.actions {
    display: flex;
    gap: 10px;
}
.actions button {
    flex: 1;
    cursor: pointer;
}
.save-btn { background: #4caf50; color: white; border: none; padding: 5px;}
.cancel-btn { background: var(--bg-secondary); border: none; padding: 5px; color: var(--text-primary); }
</style>
