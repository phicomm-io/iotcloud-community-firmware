/* Run with NODE_PATH pointing to a jsdom installation; no network/device needed. */
const {JSDOM}=require('jsdom');
const fs=require('fs');
const assert=require('node:assert/strict');
const flush=()=>new Promise(resolve=>setImmediate(resolve));
(async()=>{for(const product of ['A1','M1']){
 const html=fs.readFileSync(product+'/web/index.html','utf8');
 const dom=new JSDOM(html,{runScripts:'outside-only',url:'http://device.local'}),w=dom.window;
 w.TextEncoder=require("node:util").TextEncoder;
 const calls=[];
 const fixtures={
 '/binding/config':{revision:1,group:'',group_force_mqtt:false,group_recovery:true,group_timeout_ms:1000,group_recovery_seconds:60,peers:[],members:[]},
 '/linkage/config':{revision:1,enabled:0,mode:2,metrics:1,statistic:0,kp:20,ki:.02,kd:0},
 '/light/advanced':{revision:1,effect:4,segments:3,follow_fan:0,quality_pm:35,quality_hcho:.08,segment:[{source:0,display:2,minimum:-10,maximum:50,suitable_low:18,suitable_high:26,colors:[[0,0,255],[0,255,0],[255,0,0]]}]},
 '/schedule/config':{revision:1,events:[]},'/sleep/config':{revision:1,windows:[]},'/time/config':{revision:1,ntp_host:'pool.ntp.org',timezone_minutes:480},
 '/binding/status':{available:1,peers:[]},
 '/status':{name:product,version:'test',red:0,green:0,blue:255,scan_count:0,percentage:60,mqtt_port:1883,brightness:255,brightness_requested:4,sample:{temperature:20,humidity:50,formaldehyde:.08,pm25:35},temperature:20,humidity:50,formaldehyde:.08,pm25:35,ota_state:'idle',mqtt_host:'',mqtt_user:'',ssid:''}
 };
 w.fetch=async(path,options={})=>{calls.push([path,options.body?JSON.parse(options.body):null]);if(options.body&&fixtures[path])fixtures[path]={...JSON.parse(options.body),revision:2};return {ok:true,status:200,json:async()=>JSON.parse(JSON.stringify(fixtures[path]||{message:'Saved'}))};};
 w.setTimeout=()=>0;w.confirm=()=>true;
 for(const match of html.matchAll(/<script>([\s\S]*?)<\/script>/g))w.eval(match[1]);await flush();await flush();
 const tabs=[...w.document.querySelectorAll('#featureTabs button')];assert.equal(tabs.length,6);
 let add=[...w.document.querySelectorAll('#featureFields button')].find(x=>x.textContent==='添加绑定设备');assert.ok(add);add.click();
 const macInput=[...w.document.querySelectorAll('#featureFields input')].find(x=>x.previousElementSibling?.textContent==='MAC 地址');macInput.value='11:22:33:44:55:66';macInput.dispatchEvent(new w.Event('input'));
 w.document.getElementById('featureForm').dispatchEvent(new w.Event('submit',{cancelable:true}));await flush();await flush();assert.ok(calls.some(([p,b])=>p==='/binding/config'&&b?.peers[0]?.mac==='11:22:33:44:55:66'));
 for(let i=1;i<6;i++){tabs[i].click();await flush();await flush();assert.ok(w.document.getElementById('featureFields').textContent.length);}
 tabs[3].click();await flush();add=[...w.document.querySelectorAll('#featureFields button')].find(x=>x.textContent==='添加每周定时事件');add.click();w.document.getElementById('featureForm').dispatchEvent(new w.Event('submit',{cancelable:true}));await flush();assert.ok(calls.some(([p,b])=>p==='/schedule/config'&&b?.events[0]?.days===127));
 tabs[4].click();await flush();add=[...w.document.querySelectorAll('#featureFields button')].find(x=>x.textContent==='添加睡眠时段');add.click();w.document.getElementById('featureForm').dispatchEvent(new w.Event('submit',{cancelable:true}));await flush();assert.ok(calls.some(([p,b])=>p==='/sleep/config'&&b?.windows[0]?.start===1320&&b.windows[0].end===420));
 const user="a+b.o'neil@example.com";
 w.document.getElementById('mqttMode').value='account';
 w.document.getElementById('mqttUser').value=user;
 w.document.getElementById('mqttPassword').value='p'.repeat(256);
 w.document.getElementById('mqttForm').dispatchEvent(new w.Event('submit',{cancelable:true}));await flush();await flush();
 assert.ok(calls.some(([p,b])=>p==='/mqtt/config'&&b?.user===user&&b?.password.length===256&&b?.topic_mode==='account'));
 w.document.getElementById('mqttPassword').value='';w.document.getElementById('clearMqttPassword').checked=true;
 w.document.getElementById('mqttForm').dispatchEvent(new w.Event('submit',{cancelable:true}));await flush();await flush();
 assert.ok(calls.some(([p,b])=>p==='/mqtt/config'&&b?.clear_password&&b?.password===''));
 dom.window.close();console.log(product+' WebUI tabs, binding edit, timer and cross-midnight sleep forms passed');
}})().catch(error=>{console.error(error);process.exit(1)});
