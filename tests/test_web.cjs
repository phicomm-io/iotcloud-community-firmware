/* Run with NODE_PATH pointing to a jsdom installation; no network/device needed. */
const {JSDOM}=require('jsdom');
const fs=require('fs');
const assert=require('node:assert/strict');
const flush=()=>new Promise(resolve=>setImmediate(resolve));
(async()=>{for(const product of ['A1','M1']){
 const html=require('child_process').execFileSync('python3',['common/web/page.py',product+'/web/index.html'],{encoding:'utf8'});
 const dom=new JSDOM(html,{runScripts:'outside-only',url:'http://device.local'}),w=dom.window;
 w.TextEncoder=require("node:util").TextEncoder;
 const calls=[];
 const fixtures={
 '/binding/config':{response_ids:[],revision:1,group:'',group_force_mqtt:false,group_recovery:true,group_timeout_ms:1000,group_recovery_seconds:60,peers:[],members:[]},
 '/linkage/config':{revision:1,enabled:0,mode:2,metrics:1,statistic:0,kp:20,ki:.02,kd:0},
 '/light/advanced':{revision:1,effect:4,segments:3,follow_fan:0,quality_pm:35,quality_hcho:.08,segment:[{source:0,display:2,minimum:-10,maximum:50,suitable_low:18,suitable_high:26,colors:[[0,0,255],[0,255,0],[255,0,0]]}]},
 '/schedule/config':{revision:1,events:[]},'/sleep/config':{revision:1,windows:[]},'/time/config':{revision:1,ntp_host:'pool.ntp.org',timezone_minutes:480},
 '/binding/time/config':{revision:1,ntp_host:'pool.ntp.org',timezone_minutes:480},
 '/binding/status':{available:1,peers:[]},
 '/status':{name:product,version:'test',red:0,green:0,blue:255,scan_count:0,percentage:60,mqtt_port:1883,brightness:255,brightness_requested:4,sample:{temperature:20,humidity:50,formaldehyde:.08,pm25:35},temperature:20,humidity:50,formaldehyde:.08,pm25:35,ota_state:'idle',ota_progress:0,timezone_minutes:480,ntp_host:'pool.ntp.org',mqtt_host:'',mqtt_user:'',ssid:''}
 };
 w.fetch=async(path,options={})=>{calls.push([path,options.body?JSON.parse(options.body):null]);if(options.body&&fixtures[path])fixtures[path]={...JSON.parse(options.body),revision:2};return {ok:true,status:200,json:async()=>JSON.parse(JSON.stringify(fixtures[path]||{message:'Saved'}))};};
 w.setTimeout=()=>0;w.confirm=()=>true;
 for(const match of html.matchAll(/<script>([\s\S]*?)<\/script>/g))w.eval(match[1]+(product==='M1'?'\nwindow.renderDiagnostics=renderDiagnostics;':''));await flush();await flush();
 if(product==='M1'){
  const d=w.document;
  assert.equal(d.getElementById('ntpHost').value,'pool.ntp.org');
  d.getElementById('ntpHost').value='192.168.1.2';
  d.getElementById('ntpForm').dispatchEvent(new w.Event('submit',{cancelable:true}));await flush();
  assert.ok(calls.some(([p,b])=>p==='/ntp/config'&&b?.ntp_host==='192.168.1.2'));
  d.getElementById('timezone').value='0';
  d.getElementById('timeForm').dispatchEvent(new w.Event('submit',{cancelable:true}));await flush();
  assert.ok(calls.some(([p,b])=>p==='/time/config'&&b?.timezone_minutes===0));
  w.renderDiagnostics({wifi_connected:true,mqtt_connected:false,ntp_status:'failed',ntp_last_success:null,temperature:null,mqtt_last_error:'<img src=x onerror=alert(1)>',future_field:{nested:[false,7]}});
  const diagnostic=d.getElementById('diagnosticGroups');
  assert.ok(diagnostic.textContent.includes('时间同步'));
  assert.ok(diagnostic.textContent.includes('校时失败'));
  assert.ok(diagnostic.textContent.includes('暂无数据'));
  assert.ok(diagnostic.textContent.includes('future_field'));
  assert.ok(diagnostic.textContent.includes('<img src=x onerror=alert(1)>'));
  assert.equal(diagnostic.querySelector('img'),null);
  assert.equal(JSON.parse(d.getElementById('diagnostics').textContent).mqtt_connected,false);
  const raw=d.getElementById('diagnostics').parentElement;raw.open=true;
  w.renderDiagnostics({ntp_status:'synced',ntp_last_success:1704067200});
  assert.ok(diagnostic.textContent.includes('2024-01-01 00:00:00 UTC'));
  assert.equal(raw.open,true);
  assert.ok(!d.getElementById('bindingStatus').textContent.includes('{'));
  assert.equal(d.getElementById('homePage').hidden,false);
  assert.equal(d.getElementById('settingsPage').hidden,true);
  d.getElementById('mqttUser').value='unsaved-user';
  for(const page of ['network','mqtt','device','binding','firmware','diagnostics']){
   w.location.hash='settings/'+page;w.dispatchEvent(new w.Event('hashchange'));
   assert.equal(d.getElementById('homePage').hidden,true);
   assert.deepEqual([...d.querySelectorAll('[data-settings-page]')].filter(x=>!x.hidden).map(x=>x.dataset.settingsPage),[page]);
   assert.equal(d.querySelector('.subnav [aria-current="page"]').hash,'#settings/'+page);
  }
  w.location.hash='home';w.dispatchEvent(new w.Event('hashchange'));
  assert.equal(d.getElementById('settingsPage').hidden,true);
  assert.equal(d.getElementById('mqttUser').value,'unsaved-user');
  w.location.hash='invalid';w.dispatchEvent(new w.Event('hashchange'));
  assert.equal(d.getElementById('homePage').hidden,false);
 }
 // Exercise the real upload handler: one checkbox is sufficient for raw OTA.
 const uploads=[];
 w.confirm=()=>{throw new Error('OTA must not require a second confirmation');};
 w.XMLHttpRequest=class {
  constructor(){this.upload={};}
  open(method,path){this.method=method;this.path=path;}
  setRequestHeader(){}
  send(file){uploads.push([this.method,this.path,file.name]);this.status=202;this.onload();this.onloadend();}
 };
 const firmware=w.document.getElementById('firmware');
 const submitUpload=()=>w.document.getElementById('fileForm').dispatchEvent(new w.Event('submit',{cancelable:true}));
 Object.defineProperty(firmware,'files',{configurable:true,value:[new w.File(['raw'],'ota.bin.raw')]});
 w.document.getElementById('unsignedRescue').checked=true;submitUpload();
 assert.deepEqual(uploads.pop(),['POST','/ota/fileUploadUnsigned','ota.bin.raw']);
 Object.defineProperty(firmware,'files',{configurable:true,value:[new w.File(['signed'],'ota.bin.signed')]});
 w.document.getElementById('unsignedRescue').checked=false;submitUpload();
 assert.deepEqual(uploads.pop(),['POST','/ota/fileUpload','ota.bin.signed']);
 // A raw file without the checkbox still uses the signed gate, never a fallback.
 Object.defineProperty(firmware,'files',{configurable:true,value:[new w.File(['raw'],'ota.bin.raw')]});submitUpload();
 assert.deepEqual(uploads.pop(),['POST','/ota/fileUpload','ota.bin.raw']);
 w.document.getElementById('otaUrl').value='http://host/ota.bin.signed';
 w.document.getElementById('urlForm').dispatchEvent(new w.Event('submit',{cancelable:true}));await flush();
 assert.ok(calls.some(([path,body])=>path==='/ota'&&body?.url==='http://host/ota.bin.signed'));
 if(product==='A1'){
 const tabs=[...w.document.querySelectorAll('#featureTabs button')];assert.equal(tabs.length,6);
 let add=[...w.document.querySelectorAll('#featureFields button')].find(x=>x.textContent==='添加绑定设备');assert.ok(add);add.click();
 const macInput=[...w.document.querySelectorAll('#featureFields input')].find(x=>x.previousElementSibling?.textContent==='MAC 地址');macInput.value='11:22:33:44:55:66';macInput.dispatchEvent(new w.Event('input'));
 w.document.getElementById('featureForm').dispatchEvent(new w.Event('submit',{cancelable:true}));await flush();await flush();assert.ok(calls.some(([p,b])=>p==='/binding/config'&&b?.peers[0]?.mac==='11:22:33:44:55:66'));
 for(let i=1;i<6;i++){tabs[i].click();await flush();await flush();assert.ok(w.document.getElementById('featureFields').textContent.length);}
 tabs[3].click();await flush();add=[...w.document.querySelectorAll('#featureFields button')].find(x=>x.textContent==='添加每周定时事件');add.click();w.document.getElementById('featureForm').dispatchEvent(new w.Event('submit',{cancelable:true}));await flush();assert.ok(calls.some(([p,b])=>p==='/schedule/config'&&b?.events[0]?.days===127));
 tabs[4].click();await flush();add=[...w.document.querySelectorAll('#featureFields button')].find(x=>x.textContent==='添加夜间时段');add.click();w.document.getElementById('featureForm').dispatchEvent(new w.Event('submit',{cancelable:true}));await flush();assert.ok(calls.some(([p,b])=>p==='/sleep/config'&&b?.windows[0]?.start===1320&&b.windows[0].end===420));
 }else{
  const ids=w.document.getElementById('responseIds');assert.ok(ids);
  ids.value='112233445566:aabbccddeeff';
  w.document.getElementById('responseForm').dispatchEvent(new w.Event('submit',{cancelable:true}));await flush();
  assert.ok(calls.some(([p,b])=>p==='/binding/config'&&b?.response_ids?.[0]==='112233445566:aabbccddeeff'));
  assert.equal(w.document.getElementById('featureForm'),null);
 }
 const theme=w.document.getElementById('appearance');assert.ok(theme);
 theme.value='dark';theme.dispatchEvent(new w.Event('change'));
 assert.equal(w.document.documentElement.dataset.theme,'dark');assert.equal(w.localStorage.getItem('phicomm.appearance'),'dark');
 theme.value='system';theme.dispatchEvent(new w.Event('change'));assert.equal(w.document.documentElement.hasAttribute('data-theme'),false);
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
