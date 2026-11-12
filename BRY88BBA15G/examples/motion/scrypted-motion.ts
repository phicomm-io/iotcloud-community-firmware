// Paste into a Scrypted Script; SDK and axios are supplied by Scrypted.
import sdk, { MotionSensor, ScryptedInterface } from '@scrypted/sdk';
import axios from 'axios';

const CAMERA_ID = 'REPLACE_WITH_SCRYPTED_DEVICE_ID';
const WEBHOOK = 'http://SERVER_LAN_IP:8099/motion';
const TOKEN = 'REPLACE_WITH_G3_MOTION_TOKEN';
const camera = sdk.systemManager.getDeviceById<MotionSensor>(CAMERA_ID);
if (!camera) throw new Error('Select an existing camera with MotionSensor');
let active = false;
let busy = false;
let until = 0;

async function notify() {
  if (busy || !active || Date.now() >= until) return;
  busy = true;
  try {
    await axios.post(WEBHOOK, { camera: 'door', active: true }, {
      headers: { Authorization: `Bearer ${TOKEN}` }, timeout: 15000,
    });
  } catch (error) {
    console.warn('Motion push not confirmed; the next heartbeat will query its ID');
  } finally { busy = false; }
}

camera.listen(ScryptedInterface.MotionSensor, (_source, _details, value) => {
  active = value === true;
  until = active ? Date.now() + 600000 : 0;
  if (active) void notify();
});
// Renew the same ID every five seconds while motion stays true. Do not replay
// a stale true state at script startup; wait for a fresh MotionSensor event.
setInterval(() => { if (active) void notify(); }, 5000);
