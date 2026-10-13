import { defineConfig } from "vite";
import vue from "@vitejs/plugin-vue";

export default defineConfig({
  plugins: [vue()],
  server: {
    port: 3000,
    proxy: {
      "/mqtt": {
        target: process.env.MQTT_TARGET || "ws://localhost:9001",
        ws: true,
      },
      "/api": {
        target: process.env.API_TARGET || "http://localhost:8080",
        changeOrigin: false,
      },
    },
  },
});
