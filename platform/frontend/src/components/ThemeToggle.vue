<script setup>
import { theme, toggleTheme } from '../appearance';
import AppIcon from './AppIcon.vue';
</script>
<template><button class="theme-toggle icon-button" type="button" @click="toggleTheme" :aria-label="`主题：${theme === 'auto' ? '自动' : theme === 'dark' ? '深色' : '浅色'}，点击切换`" :title="`主题：${theme === 'auto' ? '自动' : theme === 'dark' ? '深色' : '浅色'}`"><AppIcon :name="theme === 'dark' ? 'moon' : theme === 'light' ? 'sun' : 'monitor'" /></button></template>
