<script setup>
import { ref } from "vue";
import { useRouter } from "vue-router";
import { api, account } from "../account";
const username = ref(""),
  password = ref(""),
  message = ref(""),
  busy = ref(false),
  router = useRouter();
async function login() {
  busy.value = true;
  message.value = "";
  try {
    const result = await api("/auth/login", {
      method: "POST",
      body: JSON.stringify({
        username: username.value,
        password: password.value,
      }),
    });
    account.username = result.username;
    account.password = password.value;
    account.loaded = true;
    await router.push("/");
  } catch (e) {
    message.value = e.message;
  } finally {
    busy.value = false;
  }
}
</script>
<template>
  <main class="account-page">
    <h1>登录</h1>
    <p>网站与 MQTT 使用同一账号密码。</p>
    <form @submit.prevent="login">
      <label
        >用户名<input
          v-model="username"
          autocomplete="username"
          required /></label
      ><label
        >密码<input
          v-model="password"
          type="password"
          autocomplete="current-password"
          required /></label
      ><button :disabled="busy">登录</button>
    </form>
    <p role="alert">{{ message }}</p>
    <RouterLink to="/register">注册账号</RouterLink> ·
    <RouterLink to="/">访客控制台</RouterLink>
  </main>
</template>
