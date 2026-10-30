'use strict';
const {test} = require('node:test');
const assert = require('node:assert/strict');
const {createHash, webcrypto} = require('node:crypto');
const {execFileSync} = require('node:child_process');
const {readFileSync} = require('node:fs');
const {JSDOM} = require('jsdom');
const Notes = require('../site/release-notes/core.js');
const Client = require('../site/release-notes/client.js');
const Page = require('../site/release-notes/page.js');
const CandidateUI = require('./fixtures/release_candidate_ui.cjs');
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
  const calls=[];const info=await Client.readDevice('192.168.1.2','sensitive', {fetch:async(path,options)=>{calls.push({path,options});return new Response(JSON.stringify({code:'ok',data:{model:'A1',firmware_variant:'fresh-air',version:'0.9.23-123456',tmcp_version:'v2',mac:'private'}}));}});
  assert.deepEqual(info,{model:'a1',variant:'fresh-air',version:'0.9.23-123456'});
  assert.equal(calls.length,1);assert.equal(calls[0].path,'http://192.168.1.2/api/v1/info');
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
  try{await p.controller.ready;p.field('address').value='192.168.1.2';p.field('password').value='secret';p.read();assert.equal(p.field('password').value,'');
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
  assert.equal(dom.window.document.querySelector('#notes-history').hidden,false);assert.equal(dom.window.document.querySelector('#notes-channel').disabled,true);
  assert.ok(!dom.window.document.body.textContent.includes('change_refs'));dom.window.close();
  const p=mounted({locale:'en',url:'https://phicomm-io.github.io/guides/en/release-log.html?from=0.9.14&to=0.9.23&model=m1&variant=full'});
  try{await p.controller.ready;assert.equal(p.app.dataset.reportState,'complete');assert.match(p.field('result').textContent,/Bug fixes/);}finally{p.close();}
});

test('stable snapshots never request candidate metadata; candidate snapshots require an explicit local channel', async()=>{
  const f=CandidateUI.fixture(),map={...CandidateUI.resources(f.stable),...CandidateUI.resources(f.candidate)},calls=[];
  const stable=await Client.snapshot('',{fetch:fetched(map,calls),crypto:webcrypto});
  assert.equal(stable.channel,'stable');assert.equal(calls.length,3);assert.ok(calls.every(c=>!c.path.includes('/candidate/')));
  calls.length=0;
  const candidate=await Client.snapshot('',{channel:'candidate',fetch:fetched(map,calls),crypto:webcrypto});
  assert.equal(candidate.channel,'candidate');assert.equal(candidate.catalog.stable,'v0.9.27');
  assert.equal(calls.length,3);assert.ok(calls.every(c=>c.path.startsWith('/releases/notes/candidate/')));
  assert.ok(Object.isFrozen(candidate.catalog.releases));
  for(const source of ['https://phicomm.fjrcn.com','https://phicomm.fjrcn.cn','https://phicomm-io.github.io'])
    await assert.rejects(Client.snapshot(source,{channel:'candidate',fetch:()=>{throw Error('must not fetch');}}),e=>e.code==='INVALID_SOURCE');
});
test('candidate snapshots reject foreign scopes, paths and mixed hashes without trying stable or mirrors', async()=>{
  const f=CandidateUI.fixture(),path='/releases/notes/candidate/index.json';
  for(const alter of [index=>delete index.channel,index=>{index.source.scope='stable';},index=>{index.source.kind='worker';},
    index=>{index.catalog.path='/releases/index.json';},index=>{index.manifest.path='https://phicomm.fjrcn.com/manifest.json';}]){
    const map=CandidateUI.resources(f.candidate),index=JSON.parse(map[path]);alter(index);map[path]=Buffer.from(JSON.stringify(index));
    const calls=[];await assert.rejects(Client.snapshot('',{channel:'candidate',fetch:fetched(map,calls),crypto:webcrypto}),e=>e.code==='INVALID_INDEX');
    assert.equal(calls.length,1);
  }
  const map=CandidateUI.resources(f.candidate),index=JSON.parse(map[path]);map[index.catalog.path]=Buffer.from('{}');
  await assert.rejects(Client.snapshot('',{channel:'candidate',fetch:fetched(map),crypto:webcrypto}),e=>e.code==='SOURCE_MISMATCH');
  const calls=[];await assert.rejects(Client.snapshot('',{channel:'candidate',fetch:fetched({},calls),crypto:webcrypto}),e=>e.code==='CANDIDATE_UNAVAILABLE');
  assert.deepEqual(calls.map(c=>c.path),[path]);
});
test('candidate snapshot hashes also bind the channel and stable baseline declarations', async()=>{
  for(const alter of [value=>{value.manifest.supported_scopes=[];},value=>{value.catalog.stable='v1.0.0-rc';},
    value=>{value.manifest.stable='1.0.0';},value=>{value.catalog.releases=value.catalog.releases.filter(r=>!r.prerelease);}]){
    const f=CandidateUI.fixture();alter(f.candidate);
    await assert.rejects(Client.snapshot('',{channel:'candidate',fetch:fetched(CandidateUI.resources(f.candidate)),crypto:webcrypto}),e=>e.code==='SOURCE_MISMATCH');
  }
});
test('the default stable page hides candidates even if a combined snapshot is accidentally supplied', async()=>{
  const f=CandidateUI.fixture(),calls=[],p=mounted({client:{snapshot:async(source,options)=>{calls.push({source,channel:options.channel});return f.candidate;}}});
  try{await p.controller.ready;assert.equal(p.field('channel').value,'stable');assert.equal(p.field('to').value,'0.9.27');
    assert.equal(p.field('candidates').hidden,true);assert.equal(p.field('candidate-downloads').childElementCount,0);
    assert.ok(![...p.field('to').options].some(o=>o.value.includes('-rc')));assert.deepEqual(calls,[{source:'',channel:'stable'}]);
  }finally{p.close();}
});
test('explicit candidate selection uses the stable baseline, groups all four devices and shares the candidate channel', async()=>{
  const f=CandidateUI.fixture(),calls=[],p=mounted({client:{snapshot:async(source,options)=>{calls.push({source,channel:options.channel});return f[options.channel];}}});
  try{await p.controller.ready;const stableHistory=p.app.querySelector('[data-notes-channel=stable]'),history=stableHistory.innerHTML;
    assert.equal(stableHistory.hidden,false);assert.equal(p.app.querySelector('[data-notes-channel=candidate]').hidden,true);
    p.change('source','https://phicomm.fjrcn.cn');await tick();p.change('channel','candidate');await tick();
    assert.deepEqual(calls.at(-1),{source:'',channel:'candidate'});assert.equal(p.field('source').disabled,true);
    assert.deepEqual([...p.field('source').options].map(o=>o.value),['']);
    assert.equal(p.field('from').value,'0.9.27');assert.equal(p.field('to').value,'1.0.0-rc');assert.equal(p.field('candidates').hidden,false);
    assert.deepEqual([...p.field('candidate-downloads').children].map(n=>n.id),['notes-candidate-m1','notes-candidate-a1','notes-candidate-tc1','notes-candidate-daikin']);
    const links=[...p.field('candidate-downloads').querySelectorAll('a')];
    assert.equal(links.filter(a=>a.pathname.endsWith('.ota.bin.signed')).length,10);
    assert.ok(links.every(a=>!a.href.includes('phicomm.fjrcn.')));assert.equal(stableHistory.innerHTML,history);
    assert.equal(stableHistory.hidden,true);assert.equal(p.app.querySelector('[data-notes-channel=candidate]').hidden,false);
    assert.equal(p.field('history').hidden,false);
    p.submit();assert.equal(p.app.dataset.reportState,'complete');assert.equal(p.field('result').querySelectorAll('.notes-change').length,1);
    assert.equal(new URL(p.field('share').href).searchParams.get('channel'),'candidate');
    assert.ok(p.field('result').querySelector('.notes-target a').pathname.startsWith('/releases/v1.0.0-rc/'));
    p.change('channel','stable');await tick();assert.equal(p.field('candidate-downloads').childElementCount,0);assert.equal(p.field('share').hidden,true);
    assert.equal(p.field('source').disabled,false);assert.equal(p.field('source').value,'https://phicomm.fjrcn.cn');assert.equal(p.field('to').value,'0.9.27');
  }finally{p.close();}
});
test('an RC target URL alone cannot opt a stable visitor into candidate downloads', async()=>{
  const f=CandidateUI.fixture(),calls=[],p=mounted({url:'https://phicomm-io.github.io/guides/release-log.html?from=0.9.27&to=1.0.0-rc&model=m1&variant=full',
    client:{snapshot:async(_source,options)=>{calls.push(options.channel);return f[options.channel];}}});
  try{await p.controller.ready;assert.deepEqual(calls,['stable']);assert.equal(p.app.dataset.reportState,'error');
    assert.equal(p.field('candidate-downloads').childElementCount,0);assert.equal(p.field('candidates').hidden,true);
  }finally{p.close();}
});
test('candidate shares restore the explicit channel in both languages', async()=>{
  const f=CandidateUI.fixture();
  for(const locale of ['zh','en']){
    const p=mounted({locale,url:'https://phicomm-io.github.io/guides/release-log.html?channel=candidate&from=0.9.27&to=1.0.0-rc&model=m1&variant=full',
      client:{snapshot:async(_source,options)=>f[options.channel]}});
    try{await p.controller.ready;assert.equal(p.app.dataset.reportState,'complete');assert.equal(p.field('channel').value,'candidate');
      assert.equal(p.field('model').value,'m1');assert.equal(p.field('to').value,'1.0.0-rc');assert.equal(p.field('source').options.length,1);
      assert.ok([...p.field('candidate-downloads').querySelectorAll('a')].some(a=>a.pathname==='/guides/'+(locale==='en'?'en/':'')+'m1-guide.html'));
    }finally{p.close();}
  }
});
test('an unavailable candidate never falls back to stable or leaves earlier links available', async()=>{
  const f=CandidateUI.fixture(),calls=[],p=mounted({client:{snapshot:async(_source,options)=>{calls.push(options.channel);
    if(options.channel==='candidate')throw new Client.ClientError('CANDIDATE_UNAVAILABLE','No candidate');return f.stable;}}});
  try{await p.controller.ready;p.change('channel','candidate');await tick();assert.deepEqual(calls,['stable','candidate']);
    assert.equal(p.app.dataset.reportState,'error');assert.equal(p.field('submit').disabled,true);assert.equal(p.field('candidate-downloads').childElementCount,0);
    assert.match(p.field('candidate-status').textContent,/尚未提供候选版/);assert.equal(p.field('share').hidden,true);
  }finally{p.close();}
});
test('a late candidate response cannot replace a newer stable selection', async()=>{
  const f=CandidateUI.fixture(),delayed=pending(),p=mounted({client:{snapshot:async(_source,options)=>options.channel==='candidate'?delayed.promise:f.stable}});
  try{await p.controller.ready;p.change('channel','candidate');p.change('channel','stable');await tick();delayed.resolve(f.candidate);await tick();
    assert.equal(p.field('channel').value,'stable');assert.equal(p.field('to').value,'0.9.27');assert.equal(p.field('candidates').hidden,true);
    assert.equal(p.field('candidate-downloads').childElementCount,0);assert.equal(p.app.dataset.reportState,undefined);
  }finally{p.close();}
});
test('candidate download links reject foreign or traversal paths and require a signed asset identity', async()=>{
  const f=CandidateUI.fixture(),entry=f.candidate.catalog.releases.at(-1).models.m1;
  entry.downloads.raw.path='https://phicomm.fjrcn.cn/releases/v1.0.0-rc/fake.bin';
  entry.downloads.full.path='/releases/v1.0.0-rc/../stable.bin';delete entry.variants['no-frontend-resources'].downloads.signed;
  const p=mounted({url:'https://phicomm-io.github.io/guides/release-log.html?channel=candidate',client:{snapshot:async()=>f.candidate}});
  try{await p.controller.ready;const group=p.dom.window.document.getElementById('notes-candidate-m1');
    assert.equal(group.querySelectorAll('a[href$=".ota.bin.signed"]').length,1);assert.equal(group.querySelectorAll('a[href$=".all.bin"]').length,0);
    assert.ok(!group.innerHTML.includes('fjrcn'));assert.ok(!group.innerHTML.includes('../stable'));
  }finally{p.close();}
});

async function controllerHomepage({locale='zh',releases=[],fallback=false,candidateError=null,candidateSnapshot=null,nativeCatalog=null}={}) {
  const root=require('node:path').resolve(__dirname,'..'),calls=[],f=CandidateUI.fixture();
  const dom=new JSDOM(readFileSync(root+'/site/'+(locale==='en'?'index-en.html':'index.html'),'utf8'),
    {url:'https://phicomm-io.github.io/'+(locale==='en'?'en/':''),runScripts:'outside-only'});
  if(nativeCatalog){const node=dom.window.document.createElement('script');node.id='native-downloads';node.type='application/json';node.textContent=JSON.stringify(nativeCatalog);dom.window.document.head.append(node);}
  Object.assign(dom.window,{ReleaseNotes:Notes,ReleaseNotesClient:{snapshot:async(_source,options={})=>{if(options.channel==='candidate'){if(candidateError)throw candidateError;return candidateSnapshot||f.candidate;}return f.stable;}},AbortController,
    fetch:async url=>{calls.push(String(url));return {ok:!fallback||calls.length>1,status:503,json:async()=>releases};}});
  try {
    await dom.window.eval(readFileSync(root+'/site/releases.js','utf8'));
    await tick();
    return {dom,calls,close(){dom.window.close();}};
  } catch(error){dom.window.close();throw error;}
}
test('the real homepage reads strict MCU RC tags, orders candidates numerically and retains Wi-Fi stable', async()=>{
  const release=(tag_name,extra={})=>({tag_name,draft:false,prerelease:false,assets:[{name:'m1-mcu-ota.bin'},{name:'release.json'}],...extra});
  const valid=['v0.9.9','v1.0.0-rc.2','v1.0.0-rc','v0.9.10','v1.0.0-rc.10','v1.0.0-rc.0'];
  const invalid=['v01.0.0','v1.0.0-rc.01','v1.0.0-rc1','v1.0.0-RC','v1.0.0-beta.1',
    'v1.0.0-rc-abcdef','v1.0.0-abcdef','1.0.0-rc','v9007199254740992.0.0','v1.0.0-rc.9007199254740992','v1.0.0-rc\n'];
  for(const locale of ['zh','en']){
    const p=await controllerHomepage({locale,releases:[...valid.map(tag=>release(tag)),...invalid.map(tag=>release(tag)),
      release('v9.0.0',{draft:true}),release('v8.0.0',{assets:null}),null]});
    try {
      const document=p.dom.window.document,rows=[...document.querySelectorAll('#m1-mcu-versions > details')];
      assert.deepEqual(rows.map(row=>row.querySelector('summary').textContent.split(' · ')[0]),
        ['v1.0.0-rc.10','v1.0.0-rc.2','v1.0.0-rc.0','v1.0.0-rc','v0.9.10','v0.9.9']);
      assert.deepEqual(rows.map(row=>row.open),[true,false,false,false,false,false]);
      assert.ok(rows.slice(0,4).every(row=>row.querySelector('summary').textContent.includes(locale==='en'?'Development preview':'开发预览')));
      assert.ok(rows.slice(4).every(row=>row.querySelector('summary').textContent.includes(locale==='en'?'Published':'已发布')));
      assert.deepEqual([...rows[0].querySelectorAll('a')].map(a=>a.href),[
        'https://github.com/phicomm-io/phicomm_m1_mcu_atsamd20/releases/download/v1.0.0-rc.10/m1-mcu-ota.bin',
        'https://github.com/phicomm-io/phicomm_m1_mcu_atsamd20/releases/download/v1.0.0-rc.10/release.json',
        'https://github.com/phicomm-io/phicomm_m1_mcu_atsamd20/releases/tag/v1.0.0-rc.10']);
      assert.match(document.querySelector('#latest h3').textContent,/^v0\.9\.27/);
      assert.ok(!document.querySelector('#latest').textContent.includes('-rc'));
      const candidates=[...document.querySelectorAll('.device-grid .card-links a')].filter(a=>a.search.includes('channel=candidate'));
      assert.deepEqual(candidates.map(a=>new URL(a.href).searchParams.get('model')),['tc1','m1','a1','daikin']);
      assert.ok(candidates.every(a=>a.hash==='#notes-candidate-'+new URL(a.href).searchParams.get('model')));
      assert.ok(candidates.every(a=>a.pathname==='/guides/'+(locale==='en'?'en/':'')+'release-log.html'));
      assert.ok([...document.querySelectorAll('.quick-links a')].every(a=>a.classList.contains('btn')));
      assert.equal(p.calls.length,1);assert.match(p.calls[0],/\/releases\?per_page=100$/);
    } finally {p.close();}
  }
});
test('MCU snapshot fallback uses the same RC ordering and stable versions precede their own candidates', async()=>{
  const releases=['v1.0.0-rc.10','v1.0.0','v1.0.0-rc','v1.1.0-rc'].map(tag_name=>({tag_name,assets:[]}));
  const p=await controllerHomepage({locale:'en',releases,fallback:true});
  try {
    const document=p.dom.window.document;
    assert.deepEqual([...document.querySelectorAll('#m1-mcu-versions summary')].map(n=>n.textContent.split(' · ')[0]),
      ['v1.1.0-rc','v1.0.0','v1.0.0-rc.10','v1.0.0-rc']);
    assert.deepEqual(p.calls,['https://api.github.com/repos/phicomm-io/phicomm_m1_mcu_atsamd20/releases?per_page=100',
      'https://phicomm-io.github.io/guides/assets/firmware/m1-mcu/releases.json']);
    assert.match(document.getElementById('m1-mcu-status').textContent,/Showing the site snapshot/);
    assert.match(document.querySelector('#latest h3').textContent,/^v0\.9\.27/);
  } finally {p.close();}
});

// Homepage downloads must expose prereleases without changing stable/history.
test('homepage lists all ten RC targets separately from stable, including after mirror selection', async()=>{
  for(const locale of ['zh','en']) {
    const p=await controllerHomepage({locale});
    try {
      const d=p.dom.window.document,area=d.getElementById('candidate-releases');
      assert.match(area.querySelector('h3').textContent,/v1\.0\.0-rc/);
      assert.match(area.querySelector('.tag').textContent,/Beta.*RC/);
      assert.equal(area.querySelectorAll('a[href$=".ota.bin.signed"]').length,10);
      assert.deepEqual([...area.querySelectorAll('[data-download-model]')].map(n=>n.dataset.downloadModel),['m1','a1','tc1','daikin']);
      assert.equal(area.querySelectorAll('dialog').length,4);
      for(const card of area.querySelectorAll('[data-download-model]')){
        const button=card.querySelector('button'),dialog=card.querySelector('dialog');
        assert.equal(button.getAttribute('aria-controls'),dialog.id);
        assert.ok(dialog.getAttribute('aria-labelledby'));
        assert.equal(dialog.open,false);button.click();assert.equal(dialog.open,true);
        assert.ok(dialog.querySelectorAll('.firmware-variant').length>=1);
        dialog.querySelector('button').click();assert.equal(dialog.open,false);
      }
      assert.ok([...area.querySelectorAll('a')].every(a=>a.origin==='https://phicomm-io.github.io'));
      assert.match(d.querySelector('#latest h3').textContent,/^v0\.9\.27/);
      assert.ok(!d.getElementById('archive').textContent.includes('v1.0.0-rc'));
      const selector=d.getElementById('source');selector.value='https://phicomm.fjrcn.cn';
      selector.dispatchEvent(new p.dom.window.Event('change'));await tick();
      assert.equal(area.querySelectorAll('a[href$=".ota.bin.signed"]').length,10);
      assert.ok([...area.querySelectorAll('a')].every(a=>a.origin==='https://phicomm-io.github.io'));
      assert.match(d.querySelector('#latest h3').textContent,/^v0\.9\.27/);
    }finally{p.close();}
  }
});
test('candidate catalog failure leaves stable downloads usable and displays no unverified candidate links', async()=>{
  for(const code of ['CANDIDATE_UNAVAILABLE','SOURCE_MISMATCH']) {
    const p=await controllerHomepage({locale:'en',candidateError:Object.assign(Error('unavailable'),{code})});
    try {
      const d=p.dom.window.document;
      assert.equal(d.getElementById('candidate-releases').childElementCount,0);
      assert.match(d.querySelector('#latest h3').textContent,/^v0\.9\.27/);
      assert.match(d.getElementById('candidate-release-status').textContent,code==='CANDIDATE_UNAVAILABLE'?/No release candidate/:/could not be verified/);
    }finally{p.close();}
  }
});

test('S30 native downloads appear only for the matching RC, with installation-specific file guidance',async()=>{
  const nativeCatalog=JSON.parse(readFileSync(require('node:path').resolve(__dirname,'../site/native-downloads.json'),'utf8'));
  for(const locale of ['zh','en']) {
    const f=CandidateUI.fixture();CandidateUI.appendCandidate(f.candidate,'1.0.0-rc.4','1.0.0-rc',107);
    const p=await controllerHomepage({locale,candidateSnapshot:f.candidate,nativeCatalog});
    try {
      const d=p.dom.window.document,area=d.getElementById('candidate-releases');
      assert.equal(area.querySelectorAll('[data-download-model]').length,5);
      const card=area.querySelector('[data-download-model="s30"]'),dialog=card.querySelector('dialog');
      card.querySelector('button').click();assert.equal(dialog.open,true);
      assert.equal(dialog.querySelectorAll('.firmware-file').length,2);
      assert.equal(dialog.querySelectorAll('a[href$=".ota.bin.signed"],a[href$=".all.bin"]').length,0);
      assert.match(dialog.textContent,/0x11000/);assert.match(dialog.textContent,/HTTP/);
      for(const row of dialog.querySelectorAll('.firmware-file')){
        assert.match(row.textContent,/SHA-256: [a-f0-9]{64}/);
        assert.equal(row.querySelector('a').origin,'https://github.com');
        assert.ok(row.querySelector('a').pathname.includes('/v1.0.0-rc.4/'));
      }
      assert.ok(dialog.querySelector('a[href*="s30-flash.html"]'));
      dialog.querySelector('button').click();assert.equal(dialog.open,false);
    }finally{p.close();}
    const newer=multipleCandidates(),other=await controllerHomepage({locale,candidateSnapshot:newer.candidate,nativeCatalog});
    try{assert.equal(other.dom.window.document.querySelectorAll('#candidate-releases [data-download-model="s30"]').length,0);}
    finally{other.close();}
  }
});

function multipleCandidates() {
  const f=CandidateUI.fixture();
  CandidateUI.appendCandidate(f.candidate,'1.0.0-rc.1','1.0.0-rc',103);
  CandidateUI.appendCandidate(f.candidate,'1.0.0-rc.10','1.0.0-rc.2',105);
  CandidateUI.appendCandidate(f.candidate,'1.0.0-rc.2','1.0.0-rc.1',104);
  return f;
}
test('homepage displays only the latest numeric RC and explains Beta stability in both languages',async()=>{
  for(const locale of ['zh','en']) {
    const f=multipleCandidates(),p=await controllerHomepage({locale,candidateSnapshot:f.candidate});
    try {
      const d=p.dom.window.document,area=d.getElementById('candidate-releases');
      assert.equal(area.children.length,1);assert.match(area.querySelector('h3').textContent,/v1\.0\.0-rc\.10/);
      assert.equal(area.querySelectorAll('a[href$=".ota.bin.signed"]').length,10);
      assert.ok([...area.querySelectorAll('a')].filter(a=>a.pathname.startsWith('/releases/')).every(a=>a.pathname.includes('/v1.0.0-rc.10/')));
      assert.match(d.getElementById('candidate-release-status').textContent,locale==='en'?/beta testing/:/Beta 测试/);
      assert.match(d.querySelector('.version-note').textContent,locale==='en'?/beta testing/:/Beta 测试/);
      assert.match(d.querySelector('#latest h3').textContent,/^v0\.9\.27/);
    }finally{p.close();}
  }
});
test('candidate comparison hides old RC choices while accepting an installed old RC as the source',async()=>{
  for(const locale of ['zh','en']) {
    const f=multipleCandidates(),p=mounted({locale,url:'https://phicomm-io.github.io/guides/release-log.html?channel=candidate&from=1.0.0-rc.1&to=1.0.0-rc.10&model=m1&variant=full',client:{snapshot:async()=>f.candidate}});
    try {
      await p.controller.ready;
      assert.deepEqual([...p.field('to').options].map(o=>o.value),['1.0.0-rc.10']);
      assert.equal(p.field('from').value,'1.0.0-rc.1');assert.equal(p.app.dataset.reportState,'complete');
      assert.ok(p.field('result').querySelector('.notes-target a').pathname.includes('/v1.0.0-rc.10/'));
      assert.ok([...p.field('candidate-downloads').querySelectorAll('a')].filter(a=>a.pathname.startsWith('/releases/')).every(a=>a.pathname.includes('/v1.0.0-rc.10/')));
      assert.match(p.field('candidate-status').textContent,locale==='en'?/beta testing/:/Beta 测试/);
    }finally{p.close();}
  }
});
test('a withdrawn RC share requires confirmation of the latest target and supplies no old download',async()=>{
  for(const locale of ['zh','en']) for(const to of ['1.0.0-rc','v1.0.0-rc.1']) {
    const f=multipleCandidates(),p=mounted({locale,url:'https://phicomm-io.github.io/guides/release-log.html?channel=candidate&from=0.9.27&model=m1&variant=full&to='+to,client:{snapshot:async()=>f.candidate}});
    try {
      await p.controller.ready;assert.equal(p.app.dataset.reportState,'error');assert.match(p.field('status').textContent,/WITHDRAWN_CANDIDATE/);
      assert.equal(p.field('share').hidden,true);assert.equal(p.field('result').childElementCount,0);
      assert.deepEqual([...p.field('to').options].map(o=>o.value),['1.0.0-rc.10']);
      p.submit();assert.equal(p.app.dataset.reportState,'complete');
      assert.ok(p.field('result').querySelector('.notes-target a').pathname.includes('/v1.0.0-rc.10/'));
    }finally{p.close();}
  }
});


test('the committed MCU fallback snapshot exposes the published RC and preserves old releases', async()=>{
  const releases=JSON.parse(readFileSync(require('node:path').resolve(__dirname,'../docs/public/assets/firmware/m1-mcu/releases.json'),'utf8'));
  assert.equal(releases.length,5);
  assert.equal(releases[0].tag_name,'v1.0.0-rc');
  assert.equal(releases[0].prerelease,true);
  assert.deepEqual(releases.slice(1).map(r=>r.tag_name),['v0.9.4','v0.9.3','v0.9.2','v0.9.1']);
  for(const locale of ['zh-CN','en']) {
    const p=await controllerHomepage({locale,releases,fallback:true});
    try {
      const doc=p.dom.window.document;
      assert.match(doc.querySelector('#m1-mcu-versions summary').textContent,/^v1\.0\.0-rc ·/);
      const hrefs=[...doc.querySelectorAll('#m1-mcu-versions > details:first-child a')].map(a=>a.href);
      for(const name of ['m1-mcu-ota.bin','m1-mcu-ota.json','m1-mcu-debug-recovery.zip','SHA256SUMS','release.json']) {
        const url='https://github.com/phicomm-io/phicomm_m1_mcu_atsamd20/releases/download/v1.0.0-rc/'+name;
        assert.ok(releases[0].assets.some(a=>a.name===name&&a.browser_download_url===url));assert.ok(hrefs.includes(url));
      }
    } finally {p.close();}
  }
});
