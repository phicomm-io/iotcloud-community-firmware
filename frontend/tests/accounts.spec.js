import { test, expect } from "@playwright/test";
import mqtt from "mqtt";
const origin = process.env.PHICOMM_WEB_URL || "http://127.0.0.1:25173";
test("signup without MAC, cloud persistence, isolation, controls and guest preservation", async ({
  page,
  browser,
}) => {
  const user = "web_" + Date.now(),
    password = "test-password",
    errors = [];
  page.on("pageerror", (e) => errors.push(e.message));
  await page.goto("/");
  await expect(page).toHaveURL(/\/login$/);
  await page.evaluate(() =>
    localStorage.setItem(
      "ztc1_devices",
      JSON.stringify([{ mac: "001122334455", version: "v1" }]),
    ),
  );
  await page.goto("/register");
  await page.getByLabel("用户名").fill(user);
  await page.getByLabel("密码", { exact: true }).fill(password);
  await page.getByRole("button", { name: "注册", exact: true }).click();
  await expect(page.getByRole("alert")).toContainText("注册成功");
  await page.getByRole("link", { name: "登录", exact: true }).click();
  await expect(
    page.getByRole("heading", { name: "登录", exact: true }),
  ).toBeVisible();
  await page.getByLabel("用户名").fill(" " + user + " ");
  await page.getByLabel("密码", { exact: true }).fill(password);
  await page.getByRole("button", { name: "登录", exact: true }).click();
  await expect(
    page.getByRole("heading", { name: "我的云端设备" }),
  ).toBeVisible();
  await expect(page.getByRole("link", { name: "管理", exact: true })).toHaveCount(0);
  await expect(page.getByRole("button", { name: "连接", exact: true })).toHaveCount(0);
  await expect(page.getByLabel("密码", { exact: true })).toHaveCount(0);
  await page.getByLabel("MAC", { exact: true }).fill("AA:BB:CC:DD:EE:FF");
  await page.getByLabel("型号", { exact: true }).selectOption("m1");
  await page.getByRole("button", { name: "添加设备", exact: true }).click();
  await expect(
    page.getByRole("button", {
      name: "m1 · aabbccddeeff · community",
      exact: true,
    }),
  ).toBeVisible();
  await expect(
    page.getByRole("heading", { name: "MQTT 已连接", exact: true }),
  ).toBeVisible();
  const pub = mqtt.connect(
    process.env.PHICOMM_MQTT_WS || "ws://127.0.0.1:29001",
    { username: user, password, clientId: "fixture_" + Date.now() },
  );
  await new Promise((resolve, reject) => {
    pub.once("connect", resolve);
    pub.once("error", reject);
  });
  await new Promise((resolve, reject) =>
    pub.publish(
      "phicomm/m1/aabbccddeeff/availability",
      "online",
      { retain: true, qos: 1 },
      (e) => (e ? reject(e) : resolve()),
    ),
  );
  pub.publish(
    "phicomm/m1/aabbccddeeff/sensor",
    JSON.stringify({ temperature: 0, humidity: 42, formaldehyde: 0, pm25: 0 }),
    { qos: 1 },
  );
  await expect(page.getByText("0 °C", { exact: true })).toBeVisible();
  await page.getByLabel("屏幕亮度（设备请求值）").selectOption("0");
  const received = new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(Error("command not received")), 5000);
    pub.subscribe("phicomm/m1/aabbccddeeff/brightness/set");
    pub.on("message", (topic, payload, packet) => {
      if (topic.endsWith("brightness/set")) {
        clearTimeout(timer);
        resolve({ payload: String(payload), retain: packet.retain });
      }
    });
  });
  await page.getByRole("button", { name: "设置亮度" }).click();
  expect(await received).toEqual({ payload: "0", retain: false });
  pub.end(true);
  await page.reload();
  await expect(
    page.getByRole("button", {
      name: "m1 · aabbccddeeff · community",
      exact: true,
    }),
  ).toBeVisible();
  await expect(page.getByLabel("密码", { exact: true })).toHaveCount(0);
  await page.locator(".connection-settings > summary").click();
  await expect(page.getByRole("link", { name: "重新登录以恢复连接" })).toBeVisible();
  const second = await browser.newContext();
  const p = await second.newPage();
  await p.goto("/login");
  await p.getByLabel("用户名").fill(user);
  await p.getByLabel("密码", { exact: true }).fill(password);
  await p.getByRole("button", { name: "登录", exact: true }).click();
  await expect(
    p.getByRole("button", {
      name: "m1 · aabbccddeeff · community",
      exact: true,
    }),
  ).toBeVisible();
  await second.close();
  const outsider = await browser.newContext();
  const outsiderUser = user + "_other";
  const created = await outsider.request.post(origin + "/api/register", {
    headers: { Origin: origin },
    data: { username: outsiderUser, password },
  });
  expect(created.ok()).toBeTruthy();
  const logged = await outsider.request.post(origin + "/api/auth/login", {
    headers: { Origin: origin },
    data: { username: outsiderUser, password },
  });
  expect(logged.ok()).toBeTruthy();
  const own = await outsider.request.get(origin + "/api/devices");
  expect(await own.json()).toEqual([]);
  const bind = await outsider.request.post(origin + "/api/devices", {
    headers: { Origin: origin },
    data: { mac: "aabbccddeeff", model: "m1", version: "community" },
  });
  expect(bind.status()).toBe(409);
  await outsider.close();
  page.once("dialog", (d) => d.accept());
  await page.getByRole("button", { name: "解绑", exact: true }).click();
  await expect(page.getByText("尚无设备", { exact: true })).toBeVisible();
  await page.goto("/admin");
  await expect(page).not.toHaveURL(/\/admin$/);
  expect((await page.request.get(origin + "/api/admin/users")).status()).toBe(403);
  await page.getByRole("button", { name: "退出", exact: true }).click();
  await expect(page.getByRole("heading", { name: "登录", exact: true })).toBeVisible();
  await page.getByRole("link", { name: /进入访客模式/ }).click();
  await expect(
    page.getByRole("heading", { name: "本地设备", exact: true }),
  ).toBeVisible();
  await expect(
    page.getByRole("button", {
      name: "tc1(a1) · 001122334455 · v1",
      exact: true,
    }),
  ).toBeVisible();
  expect(errors).toEqual([]);
});

test("a1 and tc1(a1) protocol controls use actual MQTT topics", async ({
  page,
}) => {
  const user = "models_" + Date.now(),
    password = "test-model-password";
  await page.goto("/register");
  await page.getByLabel("用户名").fill(user);
  await page.getByLabel("密码", { exact: true }).fill(password);
  await page.getByRole("button", { name: "注册", exact: true }).click();
  await expect(page.getByRole("alert")).toContainText("注册成功");
  await page.goto("/login");
  await page.getByLabel("用户名").fill(user);
  await page.getByLabel("密码", { exact: true }).fill(password);
  await page.getByRole("button", { name: "登录", exact: true }).click();
  await expect(
    page.getByRole("heading", { name: "我的云端设备" }),
  ).toBeVisible();
  const add = async (mac, model) => {
    await page.getByLabel("MAC", { exact: true }).fill(mac);
    await page.getByLabel("型号", { exact: true }).selectOption(model);
    const [response] = await Promise.all([page.waitForResponse(r => r.url().endsWith("/api/devices") && r.request().method() === "POST"), page.getByRole("button", { name: "添加设备", exact: true }).click()]);
    expect(response.ok()).toBeTruthy();
    await expect(
      page.getByRole("button", { name: new RegExp(mac.toLowerCase()) }),
    ).toBeVisible();
    await page.getByRole("button", { name: new RegExp(mac.toLowerCase()) }).click();
  };
  const connect = async () => {
    await expect(
      page.getByRole("heading", { name: "MQTT 已连接", exact: true }),
    ).toBeVisible();
    const pub = mqtt.connect(
      process.env.PHICOMM_MQTT_WS || "ws://127.0.0.1:29001",
      { username: user, password, clientId: "model_fixture_" + Date.now() },
    );
    await new Promise((resolve, reject) => {
      pub.once("connect", resolve);
      pub.once("error", reject);
    });
    return pub;
  };
  const receive = async (pub, topic, click) => {
    await new Promise((resolve, reject) =>
      pub.subscribe(topic, (e) => (e ? reject(e) : resolve())),
    );
    const message = new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(Error("command timeout")), 5000);
      pub.once("message", (_, payload, packet) => {
        clearTimeout(timer);
        resolve({ payload: String(payload), retain: packet.retain });
      });
    });
    await click();
    return message;
  };
  await add("aa1122334466", "a1");
  let pub = await connect();
  pub.publish("phicomm/a1/aa1122334466/fan/state", "OFF", { retain: true });
  pub.publish("phicomm/a1/aa1122334466/fan/percentage/state", "0", {
    retain: true,
  });
  await expect(
    page.getByText("风扇：OFF · 转速：0%", { exact: true }),
  ).toBeVisible();
  expect(
    await receive(pub, "phicomm/a1/aa1122334466/fan/set", () =>
      page.getByRole("button", { name: "切换风扇" }).click(),
    ),
  ).toEqual({ payload: "ON", retain: false });
  const light = await receive(pub, "phicomm/a1/aa1122334466/light/set", () =>
    page.getByRole("button", { name: "开启灯光" }).click(),
  );
  expect(JSON.parse(light.payload)).toEqual({
    state: "ON",
    brightness: 255,
    color: { r: 255, g: 255, b: 255 },
  });
  expect(light.retain).toBe(false);
  pub.end(true);
  await add("aa1122334477", "tc1");
  pub = await connect();
  pub.publish(
    "homeassistant/switch/AA1122334477/total_socket/state",
    "set total_socket AA1122334477 0",
    { retain: true },
  );
  expect(
    await receive(pub, "device/ztc1/AA1122334477/set", () =>
      page.getByRole("button", { name: "⚡ 总开关", exact: true }).click(),
    ),
  ).toEqual({ payload: "set total_socket AA1122334477 1", retain: false });
  pub.end(true);
});

test("admin structured account and device management", async ({ page, browser }) => {
  test.skip(
    !process.env.PHICOMM_TEST_ADMIN_FILE,
    "private Compose test admin secret required",
  );
  const { readFile } = await import("node:fs/promises");
  const secret = (
    await readFile(process.env.PHICOMM_TEST_ADMIN_FILE, "utf8")
  ).trim();
  const user = "admin_test_" + Date.now();
  await page.goto("/login");
  await page.getByLabel("用户名").fill("admin");
  await page.getByLabel("密码", { exact: true }).fill(secret);
  await page.getByRole("button", { name: "登录", exact: true }).click();
  await expect(page.getByRole("heading", { name: "MQTT 已连接", exact: true })).toBeVisible();
  await page.getByRole("link", { name: "管理", exact: true }).click();
  await expect(
    page.getByRole("heading", { name: "账号", exact: true }),
  ).toBeVisible();
  await page.getByLabel("用户名", { exact: true }).fill(user);
  await page.getByLabel("初始密码").fill("admin-created-password");
  await page.getByRole("button", { name: "创建账号" }).click();
  await expect(
    page.getByText(user + " · active", { exact: false }),
  ).toBeVisible();
  const memberContext = await browser.newContext();
  const member = await memberContext.newPage();
  await member.goto("/login");
  await member.getByLabel("用户名").fill(user);
  await member.getByLabel("密码", { exact: true }).fill("admin-created-password");
  await member.getByRole("button", { name: "登录", exact: true }).click();
  await expect(member.getByRole("heading", { name: "MQTT 已连接", exact: true })).toBeVisible();
  await expect(member.getByRole("link", { name: "管理", exact: true })).toHaveCount(0);
  const userRow = page.locator("li").filter({ hasText: user }).filter({ has: page.getByRole("button", { name: "重置密码", exact: true }) });
  page.once("dialog", d => d.accept());
  await userRow.getByRole("button", { name: "设为管理员", exact: true }).click();
  await expect(member.getByRole("link", { name: "管理", exact: true })).toBeVisible({ timeout: 10000 });
  await member.getByRole("link", { name: "管理", exact: true }).click();
  await expect(member.getByRole("heading", { name: "账号与设备管理", exact: true })).toBeVisible();
  page.once("dialog", d => d.accept());
  await userRow.getByRole("button", { name: "撤销管理权限", exact: true }).click();
  await expect(member).not.toHaveURL(/\/admin$/, { timeout: 10000 });
  expect((await memberContext.request.get(origin + "/api/admin/users")).status()).toBe(403);
  await memberContext.close();
  await page.getByLabel("账号", { exact: false }).selectOption(user);
  await page.getByLabel("MAC", { exact: true }).fill("aa9988776655");
  await page.getByLabel("型号", { exact: true }).selectOption("a1");
  await page.getByRole("button", { name: "绑定", exact: true }).click();
  await expect(
    page.getByText(user + " · a1 · aa9988776655", { exact: false }),
  ).toBeVisible();
  page.once("dialog", (d) => d.accept());
  await page.locator("li").filter({ hasText: user + " · a1 · aa9988776655" }).getByRole("button", { name: "解绑", exact: true }).click();
  await expect(
    page.getByText(user + " · a1 · aa9988776655", { exact: false }),
  ).not.toBeVisible();
  const row = page.locator("li").filter({ hasText: user });
  page.once("dialog", (d) => d.accept());
  await row.getByRole("button", { name: "删除", exact: true }).click();
  await expect(row).not.toBeVisible();
});

test("account announcement automatically appears, controls scoped device and preserves legacy same MAC", async ({page}) => {
 const {userKey}=await import("../src/deviceProtocol.js");
 const user="discover_"+Date.now(),password="discover-password",mac="123456abcdef";
 const response=await page.request.post(origin+"/api/register",{headers:{Origin:origin},data:{username:user,password}});expect(response.ok()).toBeTruthy();
 await page.goto("/login");await page.getByLabel("用户名").fill(user);await page.getByLabel("密码",{exact:true}).fill(password);await page.getByRole("button",{name:"登录",exact:true}).click();
 await expect(page.getByText("尚无设备",{exact:true})).toBeVisible();
 const pub=mqtt.connect(process.env.PHICOMM_MQTT_WS,{username:user,password,clientId:"discovery_fixture_"+Date.now()});
 try {
  await new Promise((resolve,reject)=>{pub.once("connect",resolve);pub.once("error",reject)});
  const announcement=`phicomm/discovery/${userKey(user)}/${mac}`;
  await new Promise((resolve,reject)=>pub.publish(announcement,JSON.stringify({schema:1,model:"a1",version:"community"}),{qos:1,retain:true},e=>e?reject(e):resolve()));
  await expect(page.getByText("账号主题 · 自动发现",{exact:true})).toBeVisible({timeout:15000});
  await expect(page.getByRole("heading",{name:"MQTT 已连接",exact:true})).toBeVisible();
  const base=`phicomm/a1/${userKey(user)}/${mac}`;
  await new Promise((resolve,reject)=>pub.publish(base+"/fan/state","OFF",{qos:1,retain:true},e=>e?reject(e):resolve()));
  pub.publish(base+"/fan/percentage/state","0",{qos:1,retain:true});
  await expect(page.getByText("风扇：OFF · 转速：0%",{exact:true})).toBeVisible();
  await new Promise((resolve,reject)=>pub.subscribe(base+"/fan/set",{qos:1},e=>e?reject(e):resolve()));
  const received=new Promise((resolve,reject)=>{const timer=setTimeout(()=>reject(Error("scoped command missing")),5000);pub.on("message",(topic,payload,packet)=>{if(topic===base+"/fan/set"){clearTimeout(timer);resolve({payload:String(payload),retain:packet.retain})}})});
  await page.getByRole("button",{name:"切换风扇",exact:true}).click();expect(await received).toEqual({payload:"ON",retain:false});
  await page.getByLabel("MAC",{exact:true}).fill(mac);await page.getByLabel("型号",{exact:true}).selectOption("a1");await page.getByRole("button",{name:"添加设备",exact:true}).click();
  await expect(page.getByRole("button",{name:`a1 · ${mac} · community`,exact:true})).toHaveCount(2);
  const scopedRow=page.locator("li").filter({hasText:"账号主题 · 自动发现"});page.once("dialog",d=>d.accept());await scopedRow.getByRole("button",{name:"解绑",exact:true}).click();
  await expect(page.getByText("账号主题 · 自动发现",{exact:true})).not.toBeVisible();
  pub.publish(announcement,JSON.stringify({schema:1,model:"a1"}),{qos:1,retain:true});
  await expect.poll(async()=>{const r=await page.request.get(origin+"/api/devices");return (await r.json()).length}).toBe(1);
  page.once("dialog",d=>d.accept(mac));await page.getByRole("button",{name:"恢复已解绑的账号主题设备",exact:true}).click();
  await expect(page.getByText("账号主题 · 自动发现",{exact:true})).toBeVisible();
  await page.reload();await expect(page.getByRole("button",{name:`a1 · ${mac} · community`,exact:true})).toHaveCount(2);
 } finally { pub.end(true); }
});


test("guest custom MQTT works without account API and mobile layout fits", async ({ page }) => {
  await page.goto("/");
  await expect(page).toHaveURL(/\/login$/);
  await page.route("**/api/**", route => route.abort());
  await page.getByRole("link", { name: /进入访客模式/ }).click();
  await expect(page.getByRole("heading", { name: "本地设备", exact: true })).toBeVisible();
  const address = page.getByLabel("WebSocket 地址", { exact: true });
  await expect(address).toBeEditable();
  await address.fill("ws://custom.example.test:9001/mqtt");
  await page.getByRole("button", { name: "连接", exact: true }).click();
  await page.reload();
  await expect(address).toHaveValue("ws://custom.example.test:9001/mqtt");
  await page.getByLabel("MAC", { exact: true }).fill("020000001111");
  await page.getByLabel("型号", { exact: true }).selectOption("a1");
  await page.getByRole("button", { name: "添加设备", exact: true }).click();
  await expect(page.getByRole("button", { name: "a1 · 020000001111 · community", exact: true })).toBeVisible();
  await page.setViewportSize({ width: 390, height: 844 });
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBeTruthy();
  await page.getByRole("link", { name: "登录", exact: true }).click();
  await expect(page.getByRole("heading", { name: "登录", exact: true })).toBeVisible();
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBeTruthy();
});
