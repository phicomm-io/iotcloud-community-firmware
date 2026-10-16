const {test}=require('node:test'),assert=require('node:assert/strict');
const {JSDOM}=require('jsdom'),cp=require('node:child_process'),fs=require('node:fs'),vm=require('node:vm');
const flush=()=>new Promise(r=>setImmediate(r));
const html=cp.execFileSync('python3',['common/web/page.py','TC1/http_server/web/index.html'],{encoding:'utf8'});
function setup(){
 const dom=new JSDOM(html,{runScripts:'outside-only',url:'http://device.local'}),w=dom.window;
 w.TextEncoder=TextEncoder;w.scrollTo=()=>{};
 const scripts=[...html.matchAll(/<script>([\s\S]*?)<\/script>/g)].map(x=>x[1]);
 // No background timers: drive state reads explicitly in each scenario.
 const timer=w.setTimeout.bind(w);w.setTimeout=(fn,ms)=>ms===750?timer(fn,0):0;
 let writeResolve;w.fetch=async(url,options)=>{
  const q=JSON.parse(options.body);
  if(q.action==='outlet.set')return new Promise(resolve=>{writeResolve=reply=>resolve({ok:true,json:async()=>reply});});
  return {ok:true,json:async()=>({code:'ok',data:{power_watts:0,energy_kwh:0}})};
 };
 scripts.forEach((source,i)=>{if(i===scripts.length-1)w.UI.start=dev=>{w.UI.device=dev;};vm.runInContext(source,dom.getInternalVMContext());});
 const state={socket_states:[1,1,1,1,1,1],socket_names:[],child_lock:true,report_seconds:2,powerCoefficientX100:1710};
 w.UI.device.onStatus(w.UI.acceptStatus(state));
 return {dom,w,d:w.document,state,reply:r=>writeResolve(r)};
}
test('TC1 paints target before sending completes, ignores stale polls and confirms readback',async()=>{
 const {dom,w,d,state,reply}=setup();let reads=0;
 w.UI.reload=async()=>{reads++;const raw={...state,socket_states:reads===1?state.socket_states:[0,1,1,1,1,1]};w.UI.device.onStatus(w.UI.acceptStatus(raw));return raw;};
 d.querySelector('#socket0').click();
 assert.equal(d.querySelector('#socket0').getAttribute('aria-pressed'),'false');
 assert.equal(d.querySelector('#socket0').getAttribute('aria-busy'),'true');
 assert.equal(reads,0,'write must precede confirmation read');
 w.UI.device.onStatus(w.UI.acceptStatus(state));
 assert.equal(d.querySelector('#socket0').getAttribute('aria-pressed'),'false','old poll cannot undo target');
 reply({code:'accepted',data:{}});
 for(let i=0;i<20;i++){await new Promise(r=>setTimeout(r,2));if(reads>=2)break;}
 await flush();assert.equal(reads,2);assert.equal(d.querySelector('#socket0').disabled,false);
 assert.equal(d.querySelector('#socket0').getAttribute('aria-pressed'),'false');dom.window.close();
});
test('TC1 rejected write rolls back immediately and unlocks controls',async()=>{
 const {dom,w,d,reply}=setup();w.UI.reload=async()=>assert.fail('rejected writes must not wait for readback');
 d.querySelector('#socket0').click();reply({code:'invalid',message:'rejected'});await flush();await flush();
 assert.equal(d.querySelector('#socket0').getAttribute('aria-pressed'),'true');assert.equal(d.querySelector('#socket0').disabled,false);assert.match(d.querySelector('#toasts').textContent,/rejected/);dom.window.close();
});
test('TC1 accepted write that never applies returns to actual state with an unconfirmed warning',async()=>{
 const {dom,w,d,state,reply}=setup();let tick=0;w.Date.now=()=>{tick+=21000;return tick;};
 w.UI.reload=async()=>{w.UI.acceptStatus(state);return state;};
 d.querySelector('#socket0').click();reply({code:'accepted',data:{}});
 await new Promise(r=>setTimeout(r,15));
 assert.equal(d.querySelector('#socket0').getAttribute('aria-pressed'),'true');assert.equal(d.querySelector('#socket0').disabled,false);assert.match(d.querySelector('#toasts').textContent,/not confirmed|尚未确认/);dom.window.close();
});
