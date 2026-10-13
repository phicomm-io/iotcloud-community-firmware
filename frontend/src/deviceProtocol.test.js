import test from "node:test";
import assert from "node:assert/strict";
import {
  normalizeMAC,
  localDevices,
  deviceEntry,
  topics,
  command,
  applyMessage,
} from "./deviceProtocol.js";
test("canonical identities and legacy list deduplication", () => {
  assert.equal(normalizeMAC("AA:BB:CC:DD:EE:FF"), "aabbccddeeff");
  assert.throws(() => normalizeMAC("AA:BB-CC:DD:EE:FF"));
  const storage = {
    getItem: (k) =>
      k === "ztc1_devices"
        ? JSON.stringify([
            "aa:bb:cc:dd:ee:ff",
            { mac: "AABBCCDDEEFF", version: "v2" },
          ])
        : null,
  };
  assert.deepEqual(localDevices(storage), [
    { mac: "aabbccddeeff", model: "tc1", version: "v2" },
  ]);
});
test("TC1 wire casing remains compatible without display prefix", () => {
  const d = deviceEntry({ mac: "aabbccddeeff", version: "v2" });
  assert(topics(d).includes("homeassistant/switch/AABBCCDDEEFF/+/#"));
  assert.deepEqual(command(d, { payload: "set socket aabbccddeeff 0 1" }), {
    topic: "device/ztc1/AABBCCDDEEFF/set",
    payload: "set socket AABBCCDDEEFF 0 1",
  });
  const state = {};
  assert(
    applyMessage(
      d,
      state,
      "homeassistant/switch/AABBCCDDEEFF/socket_0/state",
      "set socket AABBCCDDEEFF 0 1",
    ),
  );
  assert.equal(state.socket_0, 1);
  assert.equal(
    applyMessage(
      d,
      state,
      "homeassistant/switch/000000000000/socket_0/state",
      "set socket AABBCCDDEEFF 0 0",
    ),
    false,
  );
  assert.equal(state.socket_0, 1);
});
test("M1 freshness, numeric zero and availability text", () => {
  const d = deviceEntry({ mac: "aabbccddeeff", model: "m1" }),
    s = {};
  applyMessage(
    d,
    s,
    "phicomm/m1/aabbccddeeff/sensor",
    '{"temperature":0,"humidity":0}',
    1000,
  );
  assert.equal(s.temperature, 0);
  assert.equal(s.sensorAt, 1000);
  assert.equal(s.sensorFresh, true);
  applyMessage(d, s, "phicomm/m1/aabbccddeeff/availability", "offline");
  assert.equal(s.sensorFresh, false);
  applyMessage(d, s, "phicomm/m1/aabbccddeeff/sensor", "oops", 2000);
  assert.equal(s.sensorAt, 1000);
  assert.deepEqual(command(d, { leaf: "brightness/set", value: 0 }), {
    topic: "phicomm/m1/aabbccddeeff/brightness/set",
    payload: "0",
  });
});
test("A1 HA light payload and model isolation", () => {
  const d = deviceEntry({ mac: "aabbccddeeff", model: "a1" }),
    s = {};
  assert.equal(
    applyMessage(d, s, "phicomm/m1/aabbccddeeff/brightness/state", "4"),
    false,
  );
  applyMessage(
    d,
    s,
    "phicomm/a1/aabbccddeeff/light/state",
    '{"state":"ON","brightness":0,"color":{"r":0,"g":0,"b":0}}',
  );
  assert.equal(s.light.brightness, 0);
  assert.equal(
    command(d, { leaf: "light/set", value: { state: "OFF" } }).payload,
    '{"state":"OFF"}',
  );
  assert.throws(() => command(d, { leaf: "../../other/set", value: 1 }));
});

 test("account scoped topics preserve HA structure and isolate same MAC", async () => {
  const { userKey, deviceKey } = await import("./deviceProtocol.js");
  const alice = deviceEntry({mac:"aabbccddeeff",model:"m1",namespace:"account",username:"a+b@example.com"});
  const bob = {...alice,username:"bob"};
  assert.notEqual(deviceKey(alice),deviceKey(bob));
  assert.ok(!userKey(alice.username).includes("+"));
  const own = `phicomm/m1/${userKey(alice.username)}/${alice.mac}/sensor`;
  const s={};assert.equal(applyMessage(alice,s,own,'{"temperature":24}'),true);assert.equal(s.temperature,24);
  assert.equal(applyMessage(bob,{},own,'{"temperature":24}'),false);
  assert.equal(applyMessage(alice,{},`phicomm/m1/${alice.mac}/sensor`,'{}'),false);
  const ha = `homeassistant/sensor/${userKey(alice.username)}/m1_${alice.mac}_temperature/config`;
  assert.equal(ha.split("/").length,5);assert.equal(applyMessage(alice,s,ha,'{"name":"温度"}'),true);
  assert.equal(s.entityNames.temperature,"温度");
  assert.equal(command(alice,{leaf:"brightness/set",value:0}).topic,`phicomm/m1/${userKey(alice.username)}/${alice.mac}/brightness/set`);
  const tc1 = {...alice,model:"tc1",version:"v2"};
  assert.equal(applyMessage(tc1,s,`phicomm/tc1/${userKey(tc1.username)}/${tc1.mac}/socket_5/state`,`set socket AABBCCDDEEFF 5 1`),true);
  assert.equal(s.socket_5,1);
  assert.equal(command(tc1,{payload:"set socket aabbccddeeff 5 0"}).topic,`phicomm/tc1/${userKey(tc1.username)}/${tc1.mac}/set`);
 });
