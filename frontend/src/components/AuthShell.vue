<script setup>
import AppIcon from './AppIcon.vue';
import ThemeToggle from './ThemeToggle.vue';
</script>
<template>
 <main class="auth-page">
  <header class="auth-topbar"><RouterLink to="/login" class="brand"><span class="brand-mark"><AppIcon name="plug" :size="23" /></span><span>Phicomm<span class="brand-subtitle">COMMUNITY MQTT</span></span></RouterLink><ThemeToggle /></header>
  <div class="auth-layout">
   <section class="auth-intro">
    <span class="eyebrow">设备管理 · 随时连接</span>
    <h2>一个账号，<br />管理你的设备<span class="accent-dot">。</span></h2>
    <p class="auth-intro-copy">从一盏灯到每一个插座，让日常控制<br class="desktop-only" />和设备状态都清晰可见。</p>
    <div class="device-illustration" aria-hidden="true">
     <div class="illustration-orbit orbit-one"></div><div class="illustration-orbit orbit-two"></div>
     <div class="illustration-center"><AppIcon name="cloud" :size="38" /><span>云端同步</span></div>
     <div class="illustration-device illustration-tc1"><span class="model-icon model-tc1"><AppIcon name="plug" :size="25" /></span><span><b>tc1(a1)</b><small>智能插排</small></span><span class="status-dot online"></span></div>
     <div class="illustration-device illustration-m1"><span class="model-icon model-m1"><AppIcon name="air" :size="25" /></span><span><b>m1</b><small>空气质量</small></span><span class="status-dot online"></span></div>
     <div class="illustration-device illustration-a1"><span class="model-icon model-a1"><AppIcon name="fan" :size="25" /></span><span><b>a1</b><small>空气净化</small></span><span class="status-dot online"></span></div>
    </div>
    <div class="auth-feature"><AppIcon name="shield" :size="18" /><span>独立设备权限</span><span class="feature-divider"></span><AppIcon name="cloud" :size="18" /><span>跨浏览器同步</span></div>
   </section>
   <section class="account-page surface"><slot /></section>
  </div>
  <footer class="auth-footer">phicomm-community-mqtt <span>支持 tc1(a1) · m1 · a1</span></footer>
 </main>
</template>
