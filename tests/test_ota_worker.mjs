import {test} from 'node:test';import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';
const code=await readFile(new URL('../infra/ota-worker/src.js',import.meta.url),'utf8');
const {default:worker}=await import('data:text/javascript;base64,'+Buffer.from(code).toString('base64'));
const bytes=Buffer.from('signed firmware fixture');
const env={RELEASES:{async get(key){if(key==='stable.json')return {json:async()=>({tag:'v0.9.5',version:'0.9.5',commit:'a'.repeat(40),models:{m1:{path:'/releases/v0.9.5/test.bin',size:bytes.length,sha256:'abc'}}})};if(key==='releases/v0.9.5/test.bin')return {size:bytes.length,body:bytes,httpEtag:'"hash"',writeHttpMetadata(){}};return null;}}};
test('latest derives model and coherent version from one stable object',async()=>{const r=await worker.fetch(new Request('http://ota/ota/m1/latest.json'),env);assert.equal(r.status,200);const value=await r.json();assert.equal(value.model,'m1');assert.equal(value.version,'0.9.5');assert(value.path.startsWith('/releases/'));});
test('binary has fixed length and cannot be transformed',async()=>{for(const method of ['GET','HEAD']){const r=await worker.fetch(new Request('http://ota/releases/v0.9.5/test.bin',{method}),env);assert.equal(r.headers.get('Content-Length'),String(bytes.length));assert.match(r.headers.get('Cache-Control'),/31536000.*no-transform/);assert.equal((await r.arrayBuffer()).byteLength,method==='HEAD'?0:bytes.length);}});
test('no public writes, arbitrary R2 reads or false success on failure',async()=>{assert.equal((await worker.fetch(new Request('http://ota/',{method:'PUT',body:'x'}),env)).status,405);assert.equal((await worker.fetch(new Request('http://ota/stable.json'),env)).status,404);assert.equal((await worker.fetch(new Request('http://ota/releases/v0.9.5/missing'),env)).status,404);assert.equal((await worker.fetch(new Request('http://ota/ota/m1/latest.json'),{RELEASES:{get(){throw Error();}}})).status,503);});

test('slim manifest never falls back to the full image and stays below device limit',async()=>{
 assert.equal((await worker.fetch(new Request('http://ota/ota/m1/no-frontend-resources/latest.json'),env)).status,404);
 const models={m1:{path:'full',variants:{'no-frontend-resources':{path:'/releases/v1.0.0/phicomm_m1-no-frontend-resources.ota.bin.signed',size:100,sha256:'slim',release_log:'/releases/v1.0.0/notes.md'}}}};
 const storage={RELEASES:{get:async()=>({json:async()=>({tag:'v1.0.0',version:'1.0.0',commit:'b'.repeat(40),models})})}};
 const r=await worker.fetch(new Request('http://ota/ota/m1/no-frontend-resources/latest.json'),storage),body=await r.text();
 assert.ok(body.length<4096);const value=JSON.parse(body);assert.equal(value.firmware_variant,'no-frontend-resources');assert.equal(value.sha256,'slim');assert.equal(value.variants,undefined);
});
test('fresh-air routes require A1 and never substitute ordinary images',async()=>{
 for(const variant of ['fresh-air','fresh-air-no-frontend-resources']){
  assert.equal((await worker.fetch(new Request(`http://ota/ota/a1/${variant}/latest.json`),env)).status,404);
  const models={a1:{path:'ordinary',variants:{[variant]:{path:'/releases/v1.0.0/fresh.signed',size:100,sha256:'fresh'}}}};
  const storage={RELEASES:{get:async()=>({json:async()=>({tag:'v1.0.0',version:'1.0.0',commit:'b'.repeat(40),models})})}};
  const r=await worker.fetch(new Request(`http://ota/ota/a1/${variant}/latest.json`),storage);
  assert.equal(r.status,200);assert.equal((await r.json()).firmware_variant,variant);
  for(const model of ['m1','tc1'])assert.equal((await worker.fetch(new Request(`http://ota/ota/${model}/${variant}/latest.json`),storage)).status,404);
 }
});
