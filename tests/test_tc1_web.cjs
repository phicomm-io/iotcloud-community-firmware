const {JSDOM}=require('jsdom'),fs=require('fs'),assert=require('node:assert/strict');
const flush=()=>new Promise(r=>setImmediate(r));
(async()=>{
const html=require('child_process').execFileSync('python3',['common/web/page.py','TC1/http_server/web/index.html'],{encoding:'utf8'}),dom=new JSDOM(html,{runScripts:'outside-only',url:'http://device.local'}),w=dom.window,d=w.document,calls=[],timers=[];
const state={name:'test <img>',version:'test',wifi_connected:true,ip:'1.2.3.4',mqtt_status:'ready',mqtt_host:'broker',mqtt_user:'user',mqtt_port:1883,mqtt_topic_mode:'legacy',ssid:'home',socket_names:['one','two','three','four','five','six'],socket_states:[1,1,1,1,1,1],btnClicks:[{'1':[0,-1]}],report_seconds:2,powerCoefficientX100:1710,ota_progress:0,ota_state:'idle',uptime_seconds:1,free_memory:30000};
w.fetch=async(path,o={})=>{const data=o.body?JSON.parse(o.body):null;calls.push([path,data]);if(path==='/socket')state.socket_states=data.states.map(Number);return {ok:true,status:200,text:async()=>JSON.stringify(path==='/status'?state:path==='/power'?{power_watts:0,energy_kwh:0}:path==='/schedule/config'?{events:[]}:path==='/task'?[]:{message:'Saved'})};};w.setTimeout=fn=>{timers.push(fn);return 0;};w.confirm=()=>true;
for(const match of html.matchAll(/<script>([\s\S]*?)<\/script>/g))w.eval(match[1]);await flush();await flush();
assert.equal(d.querySelector('#title').textContent,'TC1 test <img>');assert.equal(d.querySelector('img'),null);
d.querySelector('#mqttUser').value='unsaved';
for(const page of ['network','mqtt','device','schedule','buttons','firmware','diagnostics']){w.location.hash='settings/'+page;w.dispatchEvent(new w.Event('hashchange'));await flush();assert.equal(d.querySelector('#homePage').hidden,true);assert.deepEqual([...d.querySelectorAll('[data-settings-page]')].filter(x=>!x.hidden).map(x=>x.dataset.settingsPage),[page]);}
assert.equal(d.querySelector('#mqttUser').value,'unsaved');
w.location.hash='home';w.dispatchEvent(new w.Event('hashchange'));d.querySelector('#socket0').click();await flush();await flush();assert.ok(calls.some(([p,b])=>p==='/socket'&&b?.states[0]===false));
d.querySelector('#deviceName').value='new name';d.querySelector('#nameForm').dispatchEvent(new w.Event('submit',{cancelable:true}));await flush();assert.ok(calls.some(([p,b])=>p==='/deviceName'&&b?.name==='new name'));
// A polling refresh must not re-enable controls while a write is pending.
const savedFetch=w.fetch;let releaseWrite;
w.fetch=async(path,o)=>{if(path==='/socket')await new Promise(resolve=>{releaseWrite=resolve;});return savedFetch(path,o);};
d.querySelector('#socket0').click();
assert([...d.querySelectorAll('#sockets button'),d.querySelector('#allOn'),d.querySelector('#allOff')].every(button=>button.disabled));
await timers.shift()();
assert([...d.querySelectorAll('#sockets button')].every(button=>button.disabled));
releaseWrite();await flush();await flush();w.fetch=savedFetch;
assert([...d.querySelectorAll('#sockets button')].every(button=>!button.disabled));
const uploads=[];w.XMLHttpRequest=class {constructor(){this.upload={};}open(m,p){this.path=p;}setRequestHeader(){}send(){uploads.push(this.path);this.status=202;this.onload();this.onloadend();}};
Object.defineProperty(d.querySelector('#firmware'),'files',{value:[new w.File(['raw'],'ota.bin')]});for(const unsigned of [false,true]){d.querySelector('#unsignedRescue').checked=unsigned;d.querySelector('#fileForm').dispatchEvent(new w.Event('submit',{cancelable:true}));}
assert.deepEqual(uploads,['/ota/fileUpload','/ota/fileUploadUnsigned']);assert(!html.includes('jquery')&&!html.includes('eval('));console.log('TC1 navigation, unsaved fields, XSS-safe names, controls and explicit raw OTA passed');
})().catch(e=>{console.error(e);process.exit(1)});
