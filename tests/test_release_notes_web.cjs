'use strict';
const {test} = require('node:test');
const assert = require('node:assert/strict');
const {createHash, webcrypto} = require('node:crypto');
const {execFileSync} = require('node:child_process');
const {JSDOM} = require('jsdom');
const Notes = require('../site/release-notes/core.js');
const Client = require('../site/release-notes/client.js');
const Page = require('../site/release-notes/page.js');
const fixture = JSON.parse(execFileSync('python3', ['-c', `import sys,json;sys.path[:0]=['tools/release','site'];import user_notes,notes_ui;m,c=user_notes.compile_manifest();print(json.dumps(dict(manifest=m,catalog=json.loads(c),zh=notes_ui.body(m,'zh-CN'),en=notes_ui.body(m,'en'))))`], {cwd: require('node:path').resolve(__dirname, '..'), maxBuffer: 2e6}));
const snapshot = {manifest: fixture.manifest, catalog: fixture.catalog, source: '', id: 'fixture'};
const hash = bytes => createHash('sha256').update(bytes).digest('hex');
const pending = () => {let resolve, reject; const promise = new Promise((a,b)=>{resolve=a;reject=b;});return {promise,resolve,reject};};
const tick = () => new Promise(resolve => setImmediate(resolve));
function resources() {
  const catalog = Buffer.from(JSON.stringify(fixture.catalog));
  const manifest = Buffer.from(JSON.stringify({...fixture.manifest, archive_sha256: hash(catalog)}));
  const path = '/releases/notes/manifest.' + hash(manifest) + '.json';
  const index = {schema: 1, catalog: {path:'/releases/index.json',sha256:hash(catalog),size:catalog.length}, manifest:{path,sha256:hash(manifest),size:manifest.length}};
  return {'/releases/notes/index.json': Buffer.from(JSON.stringify(index)), [path]:manifest, '/releases/index.json':catalog};
}
function fetched(map, calls = []) {return async (path, options) => {calls.push({path,options});return new Response(map[path] || 'missing', {status:map[path]?200:404});};}
function device(data, status=200) {return {fetch:async()=>new Response(JSON.stringify(data),{status})};}
function abortable() {return (_url,{signal}) => new Promise((_resolve,reject)=> {if(signal.aborted) reject(new DOMException('aborted','AbortError'));else signal.addEventListener('abort',()=>reject(new DOMException('aborted','AbortError')),{once:true});});}
function mounted({url='https://phicomm-io.github.io/guides/release-log.html', locale='zh', client={}}={}) {
  const dom = new JSDOM(fixture[locale], {url}); const app=dom.window.document.querySelector('[data-release-notes]');
  const controller = Page.mount(app,{Notes,Client:{snapshot:async()=>snapshot,readDevice:async()=>({model:'a1',variant:'full',version:'0.9.14'}),...client}});
  const field=id=>dom.window.document.getElementById('notes-'+id);
  const change=(id,value)=>{field(id).value=value;field(id).dispatchEvent(new dom.window.Event('change',{bubbles:true}));};
  const submit=()=>field('form').dispatchEvent(new dom.window.Event('submit',{bubbles:true,cancelable:true}));
  const read=()=>field('device-form').dispatchEvent(new dom.window.Event('submit',{bubbles:true,cancelable:true}));
  return {dom,app,controller,field,change,submit,read,close(){controller.destroy();dom.window.close();}};
}

test('browser verifies, freezes and reuses one consistent manifest/catalog snapshot', async()=>{
  const calls=[];const result=await Client.snapshot('',{fetch:fetched(resources(),calls),crypto:webcrypto});
  assert.equal(calls.length,3);assert.ok(Object.isFrozen(result.manifest.releases[0].targets));
  assert.equal(result.manifest.archive_sha256,hash(Buffer.from(JSON.stringify(result.catalog))));
  assert.ok(calls.every(c=>c.options.credentials==='omit'&&c.options.redirect==='error'));
});
test('a stale mirror or corrupt resource never produces a partial snapshot', async()=>{
  const map=resources();map['/releases/index.json']=Buffer.from('{}');
  await assert.rejects(Client.snapshot('',{fetch:fetched(map),crypto:webcrypto}),e=>['SOURCE_MISMATCH','DATA_TOO_LARGE'].includes(e.code));
  const other=resources();other['/releases/notes/index.json']=Buffer.from('{"schema":1}');
  await assert.rejects(Client.snapshot('',{fetch:fetched(other),crypto:webcrypto}),e=>e.code==='INVALID_INDEX');
});
test('browser rejects oversized resources', async()=>{
  const map=resources();const index=JSON.parse(map['/releases/notes/index.json']);index.manifest.size=5e6;map['/releases/notes/index.json']=Buffer.from(JSON.stringify(index));
  await assert.rejects(Client.snapshot('',{fetch:fetched(map),crypto:webcrypto}),e=>e.code==='INVALID_INDEX');
});
test('HTTP LAN pages verify release hashes without Web Crypto, including padding boundaries', async()=>{
  for (const padding of [0,1,55,56,63,64,65,127,4096]) {
    const map=resources(), index=JSON.parse(map['/releases/notes/index.json']);
    const catalog=Buffer.concat([map['/releases/index.json'],Buffer.alloc(padding,32)]);
    const manifest=Buffer.from(JSON.stringify({...fixture.manifest,archive_sha256:hash(catalog)}));
    delete map[index.manifest.path];
    index.catalog={path:'/releases/index.json',sha256:hash(catalog),size:catalog.length};
    index.manifest={path:'/releases/notes/manifest.'+hash(manifest)+'.json',sha256:hash(manifest),size:manifest.length};
    map['/releases/index.json']=catalog;map[index.manifest.path]=manifest;
    map['/releases/notes/index.json']=Buffer.from(JSON.stringify(index));
    const result=await Client.snapshot('',{fetch:fetched(map),crypto:{}});
    assert.equal(result.manifest.archive_sha256,hash(catalog));
    catalog[0]^=1;
    await assert.rejects(Client.snapshot('',{fetch:fetched(map),crypto:{}}),error=>error.code==='SOURCE_MISMATCH');
  }
});
test('device read uses one existing info request and only the real firmware identity', async()=>{
  const calls=[];const info=await Client.readDevice('10.117.152.111','sensitive', {fetch:async(path,options)=>{calls.push({path,options});return new Response(JSON.stringify({code:'ok',data:{model:'A1',firmware_variant:'fresh-air',version:'0.9.23-123456',tmcp_version:'v2',mac:'private'}}));}});
  assert.deepEqual(info,{model:'a1',variant:'fresh-air',version:'0.9.23-123456'});
  assert.equal(calls.length,1);assert.equal(calls[0].path,'http://10.117.152.111/api/v1/info');
  assert.equal(calls[0].options.method,'GET');assert.match(calls[0].options.headers.Authorization,/^Basic /);
  assert.ok(!JSON.stringify(info).includes('private'));
});
test('legacy, unauthorized, busy and incomplete identities retain explicit error codes', async()=>{
  for(const [status,code] of [[401,'DEVICE_UNAUTHORIZED'],[404,'OLD_DEVICE_API'],[405,'OLD_DEVICE_API'],[429,'DEVICE_BUSY'],[503,'DEVICE_BUSY']])
    await assert.rejects(Client.readDevice('192.168.1.2','',device({},status)),e=>e.code===code);
  await assert.rejects(Client.readDevice('192.168.1.2','',device({code:'busy'})),e=>e.code==='DEVICE_BUSY');
  for(const data of [{version:'v2',model:'a1',firmware_variant:'full'},{version:'0.9.23',model:'a1'},{tmcp_version:'v2',model:'a1',firmware_variant:'full'}])
    await assert.rejects(Client.readDevice('192.168.1.2','',device({code:'ok',data})),e=>e.code==='DEVICE_IDENTITY_INCOMPLETE');
});
test('device timeout, cancellation and network permission denial are distinct', async()=>{
  await assert.rejects(Client.readDevice('192.168.1.2','',{fetch:abortable(),timeout:5}),e=>e.code==='TIMEOUT');
  const controller=new AbortController();const waiting=Client.readDevice('192.168.1.2','',{fetch:abortable(),signal:controller.signal});controller.abort();
  await assert.rejects(waiting,e=>e.code==='REQUEST_ABORTED');
  await assert.rejects(Client.readDevice('192.168.1.2','',{fetch:async()=>{throw new DOMException('denied','NotAllowedError');}}),e=>e.code==='LOCAL_NETWORK_DENIED');
  await assert.rejects(Client.readDevice('192.168.1.2','',{fetch:async()=>{throw new TypeError('CORS');}}),e=>e.code==='NETWORK_BLOCKED');
});
test('device credentials and URL parameters cannot be smuggled into an address', ()=>{
  for(const address of ['https://example.com','http://admin:secret@192.168.1.2','192.168.1.2/?password=a','192.168.1.2/api/v1/info','file:///etc/passwd'])
    assert.throws(()=>Client.deviceURL(address),e=>e.code==='INVALID_DEVICE_ADDRESS');
  assert.equal(Client.deviceURL('[fd00::1]'),'http://[fd00::1]/api/v1/info');
});
test('manual page comparison groups changes, defaults latest and shares only the selected range', async()=>{
  const p=mounted({url:'https://phicomm-io.github.io/guides/release-log.html?password=secret&address=10.0.0.2&mac=private'});
  try {await p.controller.ready;assert.equal(p.field('to').value,'0.9.23');p.change('from','0.9.14');p.submit();
    assert.equal(p.app.dataset.reportState,'complete');assert.ok(p.field('result').querySelectorAll('.notes-change').length>5);
    const keys=[...new URL(p.field('share').href).searchParams.keys()];assert.deepEqual(keys,['from','to','model','variant']);
    assert.ok(!p.dom.window.location.href.includes('secret'));assert.equal(p.dom.window.localStorage.length,0);
  } finally {p.close();}
});
test('same version is empty, pending releases fail and model/variant switches clear invalid versions', async()=>{
  const p=mounted();try{await p.controller.ready;p.change('from','0.9.23');p.submit();assert.equal(p.app.dataset.reportState,'complete');assert.equal(p.field('result').querySelectorAll('.notes-change').length,0);
    p.change('from','0.9.5');p.submit();assert.equal(p.app.dataset.reportState,'error');assert.equal(p.field('share').hidden,true);
    p.change('variant','fresh-air');assert.equal(p.field('from').value,'');assert.equal(p.field('to').value,'0.9.23');
    p.change('model','m1');assert.equal(p.field('variant').value,'full');assert.ok(![...p.field('variant').options].some(o=>o.value==='fresh-air'));
  }finally{p.close();}
});
test('unknown targets or variants in shared URLs never fall back to a complete latest report', async()=>{
  for(const suffix of ['from=0.9.14&to=0.9.99&model=a1&variant=full','from=0.9.14&to=0.9.23&model=a1&variant=unknown']){
    const p=mounted({url:'https://phicomm-io.github.io/guides/release-log.html?'+suffix});try{await p.controller.ready;assert.equal(p.app.dataset.reportState,'error');assert.equal(p.field('share').hidden,true);}finally{p.close();}
  }
});
test('outdated release source requests cannot overwrite a newer verified source', async()=>{
  const a=pending(),b=pending();let calls=0;const p=mounted({client:{snapshot:()=>++calls===1?a.promise:b.promise}});
  try{p.change('source','https://phicomm.fjrcn.cn');b.resolve(snapshot);await tick();p.change('from','0.9.14');p.submit();const result=p.field('result').textContent;
    a.resolve({...snapshot,manifest:{...snapshot.manifest,stable:'bad'}});await p.controller.ready;assert.equal(p.field('result').textContent,result);assert.equal(p.field('to').value,'0.9.23');
  }finally{p.close();}
});
test('one-shot device reads clear the password and stale replies never replace manual selection', async()=>{
  const response=pending();let calls=0;const p=mounted({client:{readDevice:async()=>{calls++;return response.promise;}}});
  try{await p.controller.ready;p.field('address').value='10.117.152.111';p.field('password').value='secret';p.read();assert.equal(p.field('password').value,'');
    p.change('model','m1');response.resolve({model:'a1',variant:'fresh-air',version:'0.9.23'});await tick();assert.equal(calls,1);assert.equal(p.field('model').value,'m1');assert.equal(p.field('from').value,'');
  }finally{p.close();}
});
test('device identity fills current firmware with its suffix; source changes cancel old reads', async()=>{
  const p=mounted({client:{readDevice:async()=>({model:'a1',variant:'full',version:'0.9.23-123456'})}});
  try{await p.controller.ready;p.field('address').value='10.0.0.2';p.read();await tick();assert.equal(p.field('from').value,'0.9.23-123456');assert.equal(p.app.dataset.reportState,undefined);p.submit();assert.equal(p.app.dataset.reportState,'error');}finally{p.close();}
});
test('manual comparison remains available after device errors; repeated mounting does not duplicate requests', async()=>{
  let calls=0;const p=mounted({client:{readDevice:async()=>{calls++;throw new Client.ClientError('DEVICE_UNAUTHORIZED','bad password');}}});
  try{await p.controller.ready;assert.equal(Page.mount(p.app),p.controller);p.read();await tick();assert.equal(calls,1);assert.match(p.field('device-status').textContent,/密码/);p.change('from','0.9.14');p.submit();assert.equal(p.app.dataset.reportState,'complete');}finally{p.close();}
});
test('English and no-JavaScript history never expose excluded or pending raw text', async()=>{
  const dom=new JSDOM(fixture.en);assert.match(dom.window.document.querySelector('#notes-history').textContent,/No new user-visible changes/);
  assert.equal(dom.window.document.querySelector('#notes-history').hidden,false);assert.ok(!dom.window.document.body.textContent.includes('change_refs'));dom.window.close();
  const p=mounted({locale:'en',url:'https://phicomm-io.github.io/guides/en/release-log.html?from=0.9.14&to=0.9.23&model=m1&variant=full'});
  try{await p.controller.ready;assert.equal(p.app.dataset.reportState,'complete');assert.match(p.field('result').textContent,/Bug fixes/);}finally{p.close();}
});
