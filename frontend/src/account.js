import { reactive } from "vue";
export const account = reactive({
  username: null,
  password: "",
  loaded: false,
});
export async function api(path, options = {}) {
  const response = await fetch(`/api${path}`, {
    credentials: "same-origin",
    ...options,
    headers: { "Content-Type": "application/json", ...options.headers },
  });
  const data = await response.json().catch(() => ({}));
  if (!response.ok) {
    if (response.status === 401 && !path.startsWith("/admin")) {
      account.username = null;
      account.password = "";
      window.dispatchEvent(new Event("phicomm-session-expired"));
    }
    throw new Error(
      data.error || data.message || `请求失败 (${response.status})`,
    );
  }
  return data;
}
export async function restoreAccount() {
  if (account.loaded) return;
  try {
    const data = await api("/auth/me");
    account.username = data.username;
  } catch {
    /* Guest access remains available. */
  }
  account.loaded = true;
}
