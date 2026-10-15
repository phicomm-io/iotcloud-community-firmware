<script setup>
import { ref, nextTick, onBeforeUnmount, onMounted } from 'vue'
import QRCode from 'qrcode'
import { Html5QrcodeScanner, Html5Qrcode } from 'html5-qrcode'

const emit = defineEmits(['config-imported'])

const showModal = ref(false)
const activeTab = ref('export') // 'export', 'scan', 'paste'
const exportText = ref('')
const shareUrl = ref('')
const importText = ref('')
const qrCanvas = ref(null)
const scanError = ref('')
const importStatus = ref('')
const copySuccess = ref(false)

let html5QrcodeScanner = null

const encodeToBase64Url = (str) => {
    const bytes = new TextEncoder().encode(str)
    let binary = ''
    bytes.forEach((b) => { binary += String.fromCharCode(b) })
    return btoa(binary)
        .replace(/\+/g, '-')
        .replace(/\//g, '_')
        .replace(/=+$/g, '')
}

const decodeFromBase64Url = (str) => {
    const padded = str.replace(/-/g, '+').replace(/_/g, '/')
    const padLen = (4 - (padded.length % 4)) % 4
    const base64 = padded + '='.repeat(padLen)
    const binary = atob(base64)
    const bytes = Uint8Array.from(binary, (c) => c.charCodeAt(0))
    return new TextDecoder().decode(bytes)
}

const copyToClipboard = async (text) => {
    try {
        // Try modern API first
        if (navigator.clipboard && window.isSecureContext) {
            await navigator.clipboard.writeText(text)
        } else {
            // Fallback to older method
            const textarea = document.createElement('textarea')
            textarea.value = text
            textarea.style.position = 'fixed'
            textarea.style.opacity = '0'
            document.body.appendChild(textarea)
            textarea.select()
            document.execCommand('copy')
            document.body.removeChild(textarea)
        }
        copySuccess.value = true
        setTimeout(() => { copySuccess.value = false }, 2000)
    } catch (err) {
        console.error('Failed to copy:', err)
        alert('复制失败，请重试')
    }
}

const buildShareUrl = (jsonStr) => {
    const encoded = encodeToBase64Url(jsonStr)
    const base = `${window.location.origin}${window.location.pathname}`
    return `${base}?cfg=${encoded}`
}

const clearUrlConfigParam = () => {
    const url = new URL(window.location.href)
    if (url.searchParams.has('cfg')) {
        url.searchParams.delete('cfg')
        window.history.replaceState({}, '', url.toString())
    }
}

const close = () => {
  stopScanner()
  showModal.value = false
  importStatus.value = ''
}

const stopScanner = () => {
    if (html5QrcodeScanner) {
        try {
             html5QrcodeScanner.clear().catch(err => console.error(err))
        } catch(e) {}
        html5QrcodeScanner = null
    }
}

const getConfigData = () => {
    const data = {
        timestamp: Date.now(),
        mqtt: JSON.parse(localStorage.getItem('mqtt_config') || '{}'),
        devices: JSON.parse(localStorage.getItem('ztc1_devices') || '[]'),
        remarks: {},
        auto_connect: localStorage.getItem('mqtt_auto_connect')
    }
    
    // Collect remarks
    for (let i = 0; i < localStorage.length; i++) {
        const key = localStorage.key(i)
        if (key && key.startsWith('ztc1_remarks_')) {
            try {
               data.remarks[key] = JSON.parse(localStorage.getItem(key))
            } catch(e) {}
        }
    }
    return JSON.stringify(data)
}

const switchTab = async (tab) => {
    // Cleanup prev tab
    if (activeTab.value === 'scan') {
        stopScanner()
    }
    
    activeTab.value = tab
    importStatus.value = ''
    
    if (tab === 'export') {
        const str = getConfigData()
        exportText.value = str
        shareUrl.value = buildShareUrl(str)
        await nextTick()
        if (qrCanvas.value) {
            QRCode.toCanvas(qrCanvas.value, str, { width: 200, margin: 2 }, (error) => {
                if (error) console.error(error)
            })
        }
    } else if (tab === 'scan') {
        await nextTick()
        startScanner()
    }
}

const startScanner = () => {
    // Use a small timeout to ensure DOM is ready
    setTimeout(() => {
        const onScanSuccess = (decodedText, decodedResult) => {
            // handle the scanned code as you like, for example:
            console.log(`Code matched = ${decodedText}`, decodedResult)
            stopScanner()
            processImport(decodedText)
        }

        const onScanFailure = (error) => {
            // handle scan failure, usually better to ignore and keep scanning.
            // console.warn(`Code scan error = ${error}`);
        }

        html5QrcodeScanner = new Html5QrcodeScanner(
            "reader",
            { fps: 10, qrbox: { width: 250, height: 250 } },
            /* verbose= */ false
        )
        html5QrcodeScanner.render(onScanSuccess, onScanFailure)
    }, 100)
}

const processImport = (jsonStr) => {
    try {
        const data = JSON.parse(jsonStr)
        
        // Basic Validation
        if (!data.mqtt && !data.devices) {
            throw new Error('Invalid Configuration Format')
        }
        
        // Restore
        if (data.mqtt) {
            localStorage.setItem('mqtt_config', JSON.stringify(data.mqtt))
        }
        
        if (data.devices) {
            localStorage.setItem('ztc1_devices', JSON.stringify(data.devices))
            localStorage.setItem('phicomm_devices', JSON.stringify(data.devices))
        }
        
        if (data.auto_connect) {
            localStorage.setItem('mqtt_auto_connect', data.auto_connect)
        }
        
        if (data.remarks) {
            Object.keys(data.remarks).forEach(key => {
                localStorage.setItem(key, JSON.stringify(data.remarks[key]))
            })
        }
        
        clearUrlConfigParam()
        importStatus.value = '配置导入成功！页面即将刷新...'
        setTimeout(() => {
            close()
            emit('config-imported') // Parent can reload or we can reload page
            window.location.reload()
        }, 1500)
        
    } catch (e) {
        importStatus.value = '导入失败: 格式错误'
        console.error(e)
    }
}

onBeforeUnmount(() => {
    stopScanner()
})

const tryImportFromUrl = () => {
    const params = new URLSearchParams(window.location.search)
    const cfg = params.get('cfg')
    if (!cfg) return
    try {
        const jsonStr = decodeFromBase64Url(cfg)
        processImport(jsonStr)
    } catch (e) {
        importStatus.value = '导入失败: URL 无效'
        console.error(e)
    }
}

onMounted(() => {
    tryImportFromUrl()
})

const handlePasteImport = () => {
    if(!importText.value) return
    processImport(importText.value)
}

// Expose open method
defineExpose({
    open: () => {
        showModal.value = true
        switchTab('export')
    }
})

</script>

<template>
  <Transition name="toast-fade">
    <div v-if="copySuccess" class="copy-toast">✅ 复制成功</div>
  </Transition>
  <div v-if="showModal" class="modal-overlay" @click.self="close">
    <div class="modal-content">
      <div class="modal-header">
        <h3>配置迁移</h3>
        <button class="close-btn" @click="close">×</button>
      </div>
      
      <div class="tabs">
        <button :class="{ active: activeTab === 'export' }" @click="switchTab('export')">导出二维码</button>
        <button :class="{ active: activeTab === 'scan' }" @click="switchTab('scan')">扫码导入</button>
        <button :class="{ active: activeTab === 'paste' }" @click="switchTab('paste')">文本导入</button>
      </div>
      
      <div class="tab-content">
        <!-- Export -->
        <div v-if="activeTab === 'export'" class="export-view">
            <div class="qr-container">
                <canvas ref="qrCanvas"></canvas>
            </div>
            <p>使用其他设备扫描此二维码同步配置</p>
            <textarea readonly v-model="exportText" class="code-area"></textarea>
            <button @click="() => copyToClipboard(exportText)" class="copy-btn">复制文本</button>
            <div class="share-url">
                <p>分享链接 (URL 编码)</p>
                <textarea readonly v-model="shareUrl" class="code-area"></textarea>
                <button @click="() => copyToClipboard(shareUrl)" class="copy-btn">复制链接</button>
            </div>
        </div>
        
        <!-- Scan -->
        <div v-if="activeTab === 'scan'" class="scan-view">
            <div id="reader" width="300px"></div>
            <p v-if="importStatus">{{ importStatus }}</p>
        </div>
        
        <!-- Paste -->
        <div v-if="activeTab === 'paste'" class="paste-view">
             <textarea v-model="importText" placeholder="在此粘贴配置 JSON..." class="code-area"></textarea>
             <button @click="handlePasteImport" class="primary-btn">执行导入</button>
             <p v-if="importStatus" class="status-msg">{{ importStatus }}</p>
        </div>
      </div>
    </div>
  </div>
</template>

<style scoped>
.modal-overlay {
  position: fixed;
  top: 0; left: 0; right: 0; bottom: 0;
  background: var(--overlay-bg);
  display: flex;
  justify-content: center;
  align-items: center;
  z-index: 999;
}
.modal-content {
  background: var(--bg-card);
  border-radius: 8px;
  width: 90%;
  max-width: 400px;
  max-height: 85vh;
  overflow-y: auto;
  box-shadow: 0 4px 6px var(--shadow-color);
  color: var(--text-primary);
}
.modal-header {
  padding: 15px;
  border-bottom: 1px solid var(--border-light);
  display: flex;
  justify-content: space-between;
  align-items: center;
}
.close-btn {
  background: none;
  border: none;
  font-size: 1.5em;
  cursor: pointer;
  color: var(--text-primary);
}
.tabs {
    display: flex;
    border-bottom: 1px solid var(--border-color);
}
.tabs button {
    flex: 1;
    padding: 10px;
    border: none;
    background: var(--bg-secondary);
    cursor: pointer;
    color: var(--text-primary);
}
.tabs button.active {
    background: var(--bg-card);
    font-weight: bold;
    border-bottom: 2px solid #2196f3;
}
.tab-content {
    padding: 20px;
    text-align: center;
}
.qr-container {
    margin-bottom: 15px;
}
.share-url {
    margin-top: 12px;
}
.code-area {
    width: 100%;
    height: 100px;
    font-size: 0.8em;
    padding: 5px;
    margin-top: 10px;
    border: 1px solid var(--border-color);
    border-radius: 4px;
    background: var(--bg-input);
    color: var(--text-primary);
}
.scan-view {
    min-height: 300px;
}
.primary-btn, .copy-btn {
    margin-top: 10px;
    padding: 8px 16px;
    background: #2196f3;
    color: white;
    border: none;
    border-radius: 4px;
    cursor: pointer;
}
.copy-btn {
    background: #607d8b;
}
.status-msg {
    color: #4caf50;
    margin-top: 10px;
    font-weight: bold;
}
.copy-toast {
    position: fixed;
    top: 20px;
    left: 50%;
    transform: translateX(-50%);
    background: #323232;
    color: #fff;
    padding: 10px 24px;
    border-radius: 8px;
    font-size: 0.95em;
    z-index: 10000;
    box-shadow: 0 4px 12px rgba(0,0,0,0.25);
    pointer-events: none;
}
.toast-fade-enter-active,
.toast-fade-leave-active {
    transition: opacity 0.3s ease;
}
.toast-fade-enter-from,
.toast-fade-leave-to {
    opacity: 0;
}
</style>
