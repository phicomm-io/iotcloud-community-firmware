const {test}=require('node:test'),assert=require('node:assert/strict');
const {JSDOM}=require('jsdom'),vm=require('node:vm');
test('DAIKIN uses shared metric scales, optional HCHO and expiring live data',()=>{
 const source=require('node:fs').readFileSync('DAIKIN/generated/web.h','utf8');
 const html=require('node:zlib').gunzipSync(Buffer.from(source.match(/\{([^}]+)\}/)[1].split(',').map(Number))).toString();
 const dom=new JSDOM(html,{runScripts:'outside-only',url:'http://device.local'}),w=dom.window;
 try{
  let now=100;const ticks=[];w.TextEncoder=TextEncoder;w.scrollTo=()=>{};w.setTimeout=()=>0;w.setInterval=fn=>{ticks.push(fn);return ticks.length;};
  Object.defineProperty(w.performance,'now',{value:()=>now});
  const scripts=[...html.matchAll(/<script>([\s\S]*?)<\/script>/g)].map(m=>m[1]);
  scripts.forEach((source,i)=>{if(i===scripts.length-1)w.UI.start=dev=>{w.UI.device=dev;};vm.runInContext(source,dom.getInternalVMContext());});
  w.I18N.set('en');
  const state={sensors:{pm25:{valid:true,value:18,age_ms:100},temperature:{valid:true,value:24,age_ms:100},humidity:{valid:true,value:50,age_ms:100},tvoc:{valid:true,value:60,age_ms:100}},co2_warmup_seconds_remaining:30};
  w.UI.device.onStatus(state);
  const card=key=>w.document.querySelector(`[data-sensor="${key}"]`);
  assert.equal(card('formaldehyde').hidden,true);
  assert.match(card('co2').textContent,/Warming up/);
  assert.ok(card('temperature').querySelector('.scale'));
  assert.equal(card('tvoc').querySelector('.scale'),null,'ppb has no invented mass conversion or rating');
  now+=20000;for(const fn of ticks)fn();assert.equal(card('pm25').querySelector('.num').textContent,'—');
  w.UI.device.onStatus({...state,sensors:{...state.sensors,formaldehyde:{valid:true,value:.012,age_ms:0}}});
  assert.equal(card('formaldehyde').hidden,false);assert.equal(card('formaldehyde').querySelector('.num').textContent,'0.012');
  assert.equal(w.UI.device.otaName,'phicomm_daikin.ota.bin.signed');
 }finally{dom.window.close();}
});
