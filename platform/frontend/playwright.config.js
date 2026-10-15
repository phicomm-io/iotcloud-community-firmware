import { defineConfig } from "@playwright/test";
export default defineConfig({
  testDir: "./tests",
  workers: 1,
  use: {
    baseURL: process.env.PHICOMM_WEB_URL || "http://127.0.0.1:25173",
    headless: true,
    channel: "chromium",
  },
  reporter: "list",
});
