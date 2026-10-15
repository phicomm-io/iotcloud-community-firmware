import { createApp } from 'vue'
import Root from './Root.vue'
import router from './router.js'
import './style.css'

createApp(Root).use(router).mount('#app')
