const {test}=require('node:test'),assert=require('node:assert/strict');
const {JSDOM}=require('jsdom'),cp=require('node:child_process'),vm=require('node:vm');
const flush=()=>new Promise(resolve=>setImmediate(resolve));
test('A1 light effects can be saved repeatedly without refreshing the page',async()=>{
 const html=cp.execFileSync('python3',['common/web/page.py','A1/web/index.html'],{encoding:'utf8'});
 const dom=new JSDOM(html,{runScripts:'outside-only',url:'http://device.local'}),w=dom.window;
 try {
  w.TextEncoder=TextEncoder;w.scrollTo=()=>{};w.setTimeout=()=>0;
  let revision=41,reads=0;const writes=[];
  w.fetch=async(url,options)=>{
   assert.equal(url,'/api/v1/command');const req=JSON.parse(options.body);let code='ok',data={};
   if(req.action==='light_effect.get'){
    reads++;data={effect:0,segments:1,follow_fan:0,quality_pm:35,quality_hcho:0.08,segment:[],revision};
   }else if(req.action==='light_effect.set'){
    writes.push(req.params);code=req.params.revision===revision?'accepted':'busy';
    if(code==='accepted')revision++;data={revision};
   }
   return {ok:true,json:async()=>({code,data})};
  };
  const scripts=[...html.matchAll(/<script>([\s\S]*?)<\/script>/g)].map(m=>m[1]);
  scripts.forEach((source,i)=>{
   if(i===scripts.length-1)w.UI.start=dev=>{w.UI.device=dev;};
   vm.runInContext(source,dom.getInternalVMContext());
  });
  await flush();reads=0;await w.UI.device.pages.light.show();
  const page=w.document.querySelector('[data-settings-page="light"]');
  for(const effect of [1,2,3]){
   const radio=page.querySelector(`input[name="effect"][value="${effect}"]`);
   radio.checked=true;radio.dispatchEvent(new w.Event('change'));
   const save=page.querySelector('.form-actions button');save.click();
   for(let i=0;i<5;i++)await flush();
   assert.equal(writes.at(-1).effect,effect);
   assert.equal(writes.at(-1).revision,revision-1,'each save must use the latest acknowledged revision');
   assert.equal(save.disabled,false);
  }
  assert.equal(reads,1,'the same page remains open throughout');
  assert.deepEqual(writes.map(p=>p.revision),[41,42,43]);
  assert.equal(revision,44);assert(!w.document.querySelector('#toasts').textContent.includes('busy'));
 }finally{dom.window.close();}
});
