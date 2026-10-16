/* Browser tests run the rendered firmware pages against the public command
 * schema. The pages must use /api/v1/command only, keep unsaved input while
 * navigating, escape device-supplied names, and expose the documented
 * controls (socket tiles, brightness, schedule form, OTA upload). */
const {JSDOM}=require('jsdom'),assert=require('node:assert/strict'),cp=require('node:child_process');
const flush=()=>new Promise(r=>setImmediate(r));
const PAGES={M1:['network','mqtt','device','binding','schedule','night','firmware','diagnostics','logs','maintenance'],
 A1:['network','mqtt','device','light','binding','linkage','schedule','buttons','night','firmware','diagnostics','logs','maintenance'],
 TC1:['network','mqtt','device','schedule','buttons','firmware','diagnostics','logs','maintenance']};
const ACTIONS={M1:['schedules.get','schedules.put','schedules.delete','schedules.enable','night.get','night.set','display.set','time.get','time.set'],
 A1:['schedules.get','schedules.put','schedules.delete','schedules.enable','night.get','night.set','buttons.get','buttons.set','fan.set','light.set','child_lock.set','binding.get','binding.set','binding.status.get','binding.probe','linkage.get','linkage.set','light_effect.get','light_effect.set','time.get','time.set'],
 TC1:['schedules.get','schedules.put','schedules.delete','schedules.enable','buttons.get','buttons.set','outlet.set','indicator.set','child_lock.set','time.get','time.set']};
(async()=>{for(const model of (process.env.WEB_TEST_MODEL?[process.env.WEB_TEST_MODEL]:['M1','A1','TC1'])){
 const path=model==='TC1'?'TC1/http_server/web/index.html':model+'/web/index.html';
 const html=cp.execFileSync('python3',['common/web/page.py',path],{encoding:'utf8'});
 const dom=new JSDOM(html,{runScripts:'outside-only',url:'http://device.local'}),w=dom.window,d=w.document,calls=[];
 const missing=[];w.console.warn=(...args)=>{if(args[0]==='i18n missing')missing.push(args[1]);};
 w.TextEncoder=require('node:util').TextEncoder;w.setTimeout=()=>0;w.confirm=()=>true;w.scrollTo=()=>{};
 const state={mqtt_topic_mode:model==='TC1'?'legacy':'tmcp',name:'test <img>',device_id:'aabbccddeeff',version:'test',ip:'1.2.3.4',ssid:'home',wifi_connected:true,mqtt_status:'ready',mqtt_host:'broker',mqtt_user:'user',mqtt_port:1883,
  brightness_requested:4,brightness:128,red:1,green:2,blue:3,speed:60,percentage:60,applied_speed:60,requested_speed:60,fan_on:true,fan_ready:true,panel_ready:true,
  sensor_received:true,sensor_valid:true,age_ms:1000,temperature:25,humidity:50,formaldehyde:0.02,pm25:20,time_synced:true,timezone_minutes:480,ntp_host:'pool.ntp.org',ntp_status:'synced',ntp_synced:true,
  ota_progress:0,ota_state:'idle',socket_names:['one','two','three','four','five','six'],socket_states:[1,1,1,1,1,1],report_seconds:2,powerCoefficientX100:1710,free_memory:30000};
 const actions=['status.get','capabilities.get','config.get','config.set','logs.get','ota.start',...ACTIONS[model]];
 const fixtures={'status.get':state,'capabilities.get':{model:model.toLowerCase(),actions},
  'schedules.get':{items:[],capacity:model==='TC1'?128:8,count:0,next:8},'buttons.get':{items:[]},'time.get':{timezone_minutes:480},
  'night.get':{windows:[],revision:1},'logs.get':{records:[{sequence:1,event:'boot'}],next:1,latest:1},
  'power.get':{power_watts:0,energy_kwh:0,powers:[0]},'binding.get':{response_ids:[],peers:[],members:[],group:'',revision:1},
  'binding.status.get':{average:{valid:true,sample_count:2,configured_count:2,temperature:25,humidity:50,pm25:20,formaldehyde:0.1},peers:[{mac:'112233445566',fresh:true,transport:'mqtt'}],members:[],linkage_status:'disabled',clock_valid:true,available:0,automatic_on:false,automatic_speed:0},
  'linkage.get':{enabled:0,mode:0,metrics:1,on_metrics:1,off_metrics:1,statistic:0,on_enabled:0,off_enabled:0,on_all:0,off_all:1,fixed_speed:60,min_speed:20,max_speed:100,debounce_seconds:15,min_switch_seconds:60,on_pm:50,off_pm:35,on_hcho:.1,off_hcho:.08,target_pm:35,target_hcho:.08,low_pm:35,high_pm:150,low_hcho:.08,high_hcho:.3,kp:20,ki:.02,kd:0,revision:1},
  'light_effect.get':{effect:0,segments:1,follow_fan:0,quality_pm:35,quality_hcho:.08,segment:[{source:0,display:2,minimum:-10,maximum:50,suitable_low:18,suitable_high:26,colors:[[0,0,255],[0,255,0],[255,0,0]]}],revision:1},
  'wifi.scan.get':{scanning:false,networks:[]}};
 let holdOutlet;
 w.fetch=async(url,o={})=>{assert.equal(url,'/api/v1/command');const req=JSON.parse(o.body);calls.push(req);
  assert.equal(o.method,'POST');assert.equal(typeof req.params,'object');if(!req.action.endsWith('.get'))assert(req.request_id);
  if(req.action==='outlet.set'){
   if(holdOutlet)await holdOutlet(req);
   if(req.params.index===undefined)state.socket_states.fill(Number(req.params.on));else state.socket_states[req.params.index]=Number(req.params.on);
  }
  return {ok:true,status:200,json:async()=>({code:req.action.endsWith('.get')?'ok':'accepted',data:fixtures[req.action]||{}})};};
 for(const match of html.matchAll(/<script>([\s\S]*?)<\/script>/g))require('node:vm').runInContext(match[1],dom.getInternalVMContext());
 for(let i=0;i<12;i++)await flush();
 assert.equal(d.querySelector('#title').textContent,model+' test <img>');
 assert.equal(d.querySelector('img'),null,'device-supplied names must not be injected as markup');
 if(model==='A1')assert(d.querySelector('#autoCard').textContent.includes('25.0 °C'),'homepage displays the live M1 mean even when linkage is disabled');
 assert(!d.querySelector('#spark')?.innerHTML.includes('NaN'),'single-point power chart must remain finite');
 if(model==='TC1')assert.equal(d.querySelector('#mqttMode').options.length,2);else assert.equal(d.querySelector('#mqttMode'),null,'M1/A1 only support TMCP');
 assert.equal(d.querySelector('#mqttHost').maxLength,127);
 // Navigation keeps unsaved input, and each page is reachable by hash.
 d.querySelector('#mqttUser').value='unsaved';
 for(const page of PAGES[model]){
  assert(d.querySelector('[data-settings-page="'+page+'"]'),model+' is missing page '+page);
  w.location.hash='settings/'+page;w.dispatchEvent(new w.Event('hashchange'));await flush();
  assert.equal(d.querySelector('#homePage').hidden,true);
  assert.deepEqual([...d.querySelectorAll('[data-settings-page]')].filter(x=>!x.hidden).map(x=>x.dataset.settingsPage),[page],model+' shows the wrong page for '+page);
 }
 assert.equal(d.querySelector('#mqttUser').value,'unsaved','navigation must not discard unsaved input');
 // Device name round-trips through config.set.
 d.querySelector('#deviceName').value='new name';
 d.querySelector('#nameForm').dispatchEvent(new w.Event('submit',{cancelable:true}));await flush();
 assert(calls.some(r=>r.action==='config.set'&&r.params.section==='device'&&r.params.values.name==='new name'));
 // Weekly schedule maps the weekday chips and time picker onto the wire format.
 w.location.hash='settings/schedule';w.dispatchEvent(new w.Event('hashchange'));await flush();
 const task=d.querySelector('[data-settings-page="schedule"] form');
 assert(task,'the schedule form is missing');
 // Choose "turn on speed" as the action, then submit.
 const actionRow=d.querySelector('[data-settings-page="schedule"] .seg');
 [...actionRow.querySelectorAll('button')].find(b=>b.textContent.trim().length)?.click();
 task.dispatchEvent(new w.Event('submit',{cancelable:true}));await flush();
 const put=calls.find(r=>r.action==='schedules.put');
 assert(put,'schedules.put was not sent');assert.equal(put.params.trigger,'weekly');
 assert.equal(put.params.days,127);assert.equal(put.params.minute,480);
 // Logs render the shared event list.
 w.location.hash='settings/logs';w.dispatchEvent(new w.Event('hashchange'));await flush();
 const log=d.querySelector('[data-settings-page="logs"] button');log.click();await flush();
 assert(d.querySelector('[data-settings-page="logs"] pre').textContent.includes('boot'));
 if(model==='TC1'){
  w.location.hash='#home';w.dispatchEvent(new w.Event('hashchange'));await flush();
  const before=calls.filter(r=>r.action==='outlet.set').length;
  const releases=[];holdOutlet=()=>new Promise(resolve=>releases.push(resolve));
  d.querySelector('#socket0').click();await flush();
  assert.equal(calls.filter(r=>r.action==='outlet.set').length-before,1,'one socket must produce exactly one command');
  assert(calls.some(r=>r.action==='outlet.set'&&r.params.index===0&&r.params.on===false),'a socket tile must toggle one socket');
  assert.equal(d.querySelector('#socket1').disabled,false,'another socket stays usable while the first command is pending');
  d.querySelector('#socket1').click();await flush();
  await w.UI.reload();
  for(const id of ['socket0','socket1'])assert.equal(d.querySelector('#'+id).getAttribute('aria-pressed'),'false','stale status must not overwrite pending socket output');
  for(const release of releases)release();holdOutlet=null;
  for(let i=0;i<4;i++)await flush();
  for(const id of ['socket0','socket1'])assert.equal(d.querySelector('#'+id).disabled,false,'acknowledged sockets become usable again');
  w.location.hash='settings/buttons';w.dispatchEvent(new w.Event('hashchange'));await flush();
  const key=d.querySelector('[data-settings-page="buttons"] form, [data-settings-page="buttons"] .card');
  assert(key);assert(d.querySelector('[data-settings-page="buttons"]').textContent.length>0);
 }
 if(model==='M1'){
  w.location.hash='#home';w.dispatchEvent(new w.Event('hashchange'));await flush();
  const brightness=d.querySelector('[data-settings-page="device"]')&&d.querySelector('[data-control=brightness]');
  const button=[...brightness.querySelectorAll('button')].find(b=>b.dataset.value==='2');
  button.click();await flush();
  assert(calls.some(r=>r.action==='display.set'&&r.params.brightness===50),'brightness 2 maps to 50%');
 }
 // OTA upload uses the explicit signed/unsigned endpoints.
 const uploads=[];
 w.XMLHttpRequest=class{constructor(){this.upload={};}open(m,p){this.path=p;}setRequestHeader(){}send(){uploads.push(this.path);this.status=202;this.onload();this.onloadend();}};
 const firmware=d.querySelector('#firmware');
 if(firmware){
  Object.defineProperty(firmware,'files',{value:[new w.File(['raw'],'ota.bin')]});
  for(const raw of [false,true]){d.querySelector('#unsignedRescue').checked=raw;d.querySelector('#fileForm').dispatchEvent(new w.Event('submit',{cancelable:true}));}
  assert.deepEqual(uploads,['/api/v1/ota/upload','/api/v1/ota/upload-unsigned']);
 }
 assert(!html.includes("fetch('/log'"),'the old log endpoint must not be used');
 assert.deepEqual(missing,[],'all displayed strings need a shared translation');
 console.log(model+': unified command API, navigation, typed tasks, logs, controls, safe names and explicit OTA passed');
 dom.window.close();
}})().catch(e=>{console.error(e);process.exit(1)});
