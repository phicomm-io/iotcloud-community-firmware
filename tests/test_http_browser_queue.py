"""Execute the shipped browser adapter with real Response/AbortController APIs."""
from pathlib import Path
import subprocess
import unittest
ROOT=Path(__file__).resolve().parents[1]
class HttpBrowserQueue(unittest.TestCase):
 def test_body_completion_cancellation_and_failed_request_recovery(self):
  source=r'''
const assert=require('assert'),vm=require('vm'),fs=require('fs');
(async()=>{
 const pending=[],seen=[];let active=0,maximum=0;
 const window={fetch:(url,options)=>{
  if(url!=='/api/v1/command')return Promise.resolve(new Response('outside'));
  seen.push(options.body);active++;maximum=Math.max(maximum,active);
  return new Promise((resolve,reject)=>pending.push({resolve,reject}));
 }};
 vm.runInNewContext(fs.readFileSync(process.argv[1],'utf8'),{window,Promise,Response,DOMException});
 const tick=()=>new Promise(resolve=>setImmediate(resolve));
 const a=window.fetch('/api/v1/command',{body:'first'});
 const controller=new AbortController();
 const b=window.fetch('/api/v1/command',{body:'cancelled-write',signal:controller.signal});
 const cancelled=assert.rejects(b,e=>e.name==='AbortError');
 const c=window.fetch('/api/v1/command',{body:'third'});
 await tick();assert.deepEqual(seen,['first']);
 let finish;
 pending.shift().resolve({status:200,statusText:'OK',headers:{'X-Proof':'kept'},arrayBuffer:()=>new Promise(resolve=>finish=resolve)});
 await tick();assert.deepEqual(seen,['first']); // headers do not release queue
 controller.abort();await cancelled;
 active--;finish(new TextEncoder().encode('{"code":"ok"}'));
 const reply=await a;assert.equal(reply.headers.get('X-Proof'),'kept');assert.equal((await reply.json()).code,'ok');
 await tick();assert.deepEqual(seen,['first','third']);
 active--;pending.shift().resolve({status:503,statusText:'Busy',headers:{},arrayBuffer:async()=>new TextEncoder().encode('{"code":"busy"}')});
 assert.equal((await c).status,503);
 const d=window.fetch('/api/v1/command',{body:'network-error'});const failed=assert.rejects(d,/network/);
 const e=window.fetch('/api/v1/command',{body:'after-error'});
 await tick();active--;pending.shift().reject(Error('network'));await failed;await tick();
 assert.deepEqual(seen,['first','third','network-error','after-error']);
 active--;pending.shift().resolve(new Response('recovered'));assert.equal(await (await e).text(),'recovered');
 assert.equal(maximum,1);assert.equal(active,0);
 assert.equal(await (await window.fetch('/other')).text(),'outside');
 const aborted=new AbortController();aborted.abort();await assert.rejects(window.fetch('/api/v1/command',{signal:aborted.signal}),e=>e.name==='AbortError');
 assert.equal(seen.length,4);console.log('Browser FIFO: full body before next, cancelled write never sent, errors/busy do not poison queue, no retry: OK');
})().catch(e=>{console.error(e);process.exitCode=1;});
'''
  subprocess.run(['node','-e',source,str(ROOT/'common/web/http_queue.js')],check=True)
