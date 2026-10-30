/* Shared WebUI framework: header, routing, feedback, reusable controls and the
 * settings pages every product has. Loaded after service.js (protocol) and
 * i18n.js (strings); each product page calls UI.start() with its own layout.
 * All requests go through command() in service.js; nothing here talks to a URL
 * directly except downloads, authenticated backups and OTA upload. */
(()=>{
'use strict';
const {t}=I18N;
// Native builds specialize only the product; optional capabilities stay live.
const PRODUCT=/* @device-product */null;
const mayM1=PRODUCT===null||PRODUCT==='m1';
const $=id=>document.getElementById(id);
// Remove the non-secret handshake fragment before any async device requests.
let importTicket=null,importInvalid=false;
if(location.hash.startsWith('#iotcloud-import=')){
 try{importTicket=IotProvision.readTicket(location.hash);}catch{importInvalid=true;}
 history.replaceState(null,'',location.pathname+location.search+'#settings/mqtt');
}

function h(tag,props,...kids){
 const el=document.createElement(tag);
 for(const [k,v] of Object.entries(props||{})){
  if(v==null||v===false)continue;
  if(k==='class')el.className=v;
  else if(k==='i18n'){el.dataset.i18n=v;el.textContent=t(v);}
  else if(k.startsWith('on')&&typeof v==='function')el.addEventListener(k.slice(2),v);
  else if(k in el&&!k.includes('-'))el[k]=v;
  else el.setAttribute(k,v===true?'':v);
 }
 for(const c of kids.flat(Infinity)){if(c==null||c===false)continue;el.append(c.nodeType?c:document.createTextNode(String(c)));}
 return el;
}
const ICONS={
 power:'<path d="M12 2v10"/><path d="M18.4 6.6a9 9 0 1 1-12.8 0"/>',
 fan:'<path d="M10.8 9.1C9 6.4 9.6 3 12.6 2.2c3-.8 4.5 2.4 2.6 6.4M14.9 10.8c2.7-1.8 6.1-1.2 6.9 1.8.8 3-2.4 4.5-6.4 2.6M13.2 14.9c1.8 2.7 1.2 6.1-1.8 6.9-3 .8-4.5-2.4-2.6-6.4M9.1 13.2C6.4 15 3 14.4 2.2 11.4c-.8-3 2.4-4.5 6.4-2.6"/><circle cx="12" cy="12" r="2"/>',
 plug:'<path d="M12 22v-5M9 8V2M15 8V2M18 8v5a6 6 0 0 1-12 0V8z"/>',
 air:'<path d="M17.7 7.7a2.5 2.5 0 1 1 1.8 4.3H2M9.6 4.6A2 2 0 1 1 11 8H2M12.6 19.4A2 2 0 1 0 14 16H2"/>',
 lock:'<rect x="4" y="11" width="16" height="10" rx="2"/><path d="M8 11V7a4 4 0 0 1 8 0v4"/>',
 wifi:'<path d="M5 12.6a10 10 0 0 1 14 0M8.5 16.1a5 5 0 0 1 7 0M2 8.8a15 15 0 0 1 20 0M12 20h.01"/>',
 home:'<path d="M3 10.5 12 3l9 7.5V20a1 1 0 0 1-1 1h-5v-6H9v6H4a1 1 0 0 1-1-1z"/>',
 sliders:'<path d="M4 21v-7M4 10V3M12 21v-9M12 8V3M20 21v-5M20 12V3M1 14h6M9 8h6M17 16h6"/>',
 edit:'<path d="M12 20h9M16.5 3.5a2.1 2.1 0 0 1 3 3L7 19l-4 1 1-4z"/>',
 trash:'<path d="M3 6h18M8 6V4h8v2M19 6l-1 14H6L5 6"/>',
 plus:'<path d="M12 5v14M5 12h14"/>',
 link:'<path d="M10 13a5 5 0 0 0 7.5.5l3-3a5 5 0 0 0-7-7l-1.7 1.7"/><path d="M14 11a5 5 0 0 0-7.5-.5l-3 3a5 5 0 0 0 7 7l1.7-1.7"/>',
 external:'<path d="M14 3h7v7M10 14 21 3M21 14v6a1 1 0 0 1-1 1H4a1 1 0 0 1-1-1V4a1 1 0 0 1 1-1h6"/>',
 info:'<circle cx="12" cy="12" r="10"/><path d="M12 16v-4M12 8h.01"/>',
 alert:'<path d="M10.3 3.9 1.8 18a2 2 0 0 0 1.7 3h17a2 2 0 0 0 1.7-3L13.7 3.9a2 2 0 0 0-3.4 0zM12 9v4M12 17h.01"/>',
 clock:'<circle cx="12" cy="12" r="10"/><path d="M12 6v6l4 2"/>',
 auto:'<path d="M21 12a9 9 0 0 1-15 6.7L3 16M3 12a9 9 0 0 1 15-6.7L21 8M21 3v5h-5M3 21v-5h5"/>'
};
function icon(name,cls='i'){const s=document.createElementNS('http://www.w3.org/2000/svg','svg');s.setAttribute('viewBox','0 0 24 24');s.setAttribute('class',cls);s.setAttribute('aria-hidden','true');s.innerHTML=ICONS[name]||'';return s;}

/* ---------- Feedback ---------- */
function toast(text,bad=false){
 let box=$('toasts');if(!box){box=h('div',{id:'toasts',class:'toasts',role:'status','aria-live':'polite'});document.body.append(box);}
 const el=h('div',{class:'toast'+(bad?' bad':'')},text);box.append(el);setTimeout(()=>el.remove(),bad?6000:2600);
}
async function run(button,fn,okKey='toast.accepted'){
 if(button){button.disabled=true;button.setAttribute('aria-busy','true');}
 try{const r=await fn();if(okKey)toast(t(okKey));return r;}
 catch(e){toast(e.message||String(e),true);}
 finally{if(button){button.disabled=false;button.removeAttribute('aria-busy');}}
}
function saveDraft(node,fn){return async()=>{const box=node.closest?.('[data-settings-page]'),version=box?.dataset.editVersion;const result=await fn();if(box?.dataset.editVersion===version)delete node.dataset.dirty;return result;};}
function onSubmit(form,fn,okKey){form.dataset.preserveDraft='1';form.addEventListener('submit',e=>{e.preventDefault();run(form.querySelector('button[type=submit],button:not([type])'),saveDraft(form,fn),okKey);});}
function confirmDialog({title,body,ok,danger}){
 if(typeof document.body.append!=='function'||!document.createElement('dialog').showModal)return Promise.resolve(window.confirm(title+'\n'+body));
 return new Promise(resolve=>{
  const d=h('dialog',{},h('h2',{},title),h('p',{},body),
   h('div',{class:'form-actions'},
    h('button',{class:'btn '+(danger?'danger':'primary'),type:'button',onclick:()=>{d.close();resolve(true);}},ok),
    h('button',{class:'btn ghost',type:'button',onclick:()=>d.close()},t('common.cancel'))));
  d.addEventListener('close',()=>{d.remove();resolve(false);});
  document.body.append(d);d.showModal();
 });
}

/* ---------- Controls ---------- */
function fieldRow(titleKey,...controls){return h('label',{class:'field'},h('span',{i18n:titleKey}),...controls);}
function switchRow(id,titleKey,descKey){
 return h('label',{class:'switch'},h('span',{class:'text'},h('b',{i18n:titleKey}),descKey&&h('span',{i18n:descKey})),h('input',{type:'checkbox',role:'switch',id}));
}
function seg(options,onPick,{full=false,aria}={}){
 const label=k=>typeof k==='function'?k():I18N.has(k)?t(k):k;
 const box=h('div',{class:'seg'+(full?' full':''),role:'group','aria-label':aria||null});
 for(const [value,key] of options)box.append(h('button',{type:'button','data-value':String(value),'aria-pressed':'false',onclick:()=>{box.set(value);onPick(value);}},label(key)));
 box.set=v=>{for(const b of box.children)b.setAttribute('aria-pressed',String(b.dataset.value===String(v)));box.value=v;};
 box.relabel=()=>options.forEach(([,key],i)=>{box.children[i].textContent=label(key);});
 I18N.onChange(box.relabel);
 return box;
}
const DAY_BITS=[1,2,4,8,16,32,64];
function daysPicker(name,mask=127){
 const box=h('div',{class:'days',role:'group'});
 DAY_BITS.forEach((bit,i)=>box.append(h('label',{},h('input',{type:'checkbox',name,value:bit,checked:!!(mask&bit)}),h('span',{'data-i18n':'day.short.'+i},t('day.short.'+i)))));
 box.mask=()=>[...box.querySelectorAll('input')].reduce((m,i)=>m|(i.checked?Number(i.value):0),0);
 box.setMask=m=>box.querySelectorAll('input').forEach(i=>{i.checked=!!(m&Number(i.value));});
 return box;
}
function presetDays(picker){
 return h('div',{class:'row small'},[[127,'day.every'],[31,'day.weekdays'],[96,'day.weekends']].map(([m,k])=>h('button',{type:'button',class:'btn ghost',style:'min-height:28px;padding:2px 8px',onclick:()=>{picker.setMask(m);picker.dispatchEvent(new Event('change',{bubbles:true}));}},h('span',{i18n:k}))));
}
function daysText(mask){
 if(!mask)return t('day.once');if(mask===127)return t('day.every');if(mask===31)return t('day.weekdays');if(mask===96)return t('day.weekends');
 return DAY_BITS.map((b,i)=>mask&b?t('day.short.'+i):null).filter(Boolean).join(I18N.lang==='zh-CN'?'、':', ');
}
const hhmm=m=>String(Math.floor(m/60)%24).padStart(2,'0')+':'+String(m%60).padStart(2,'0');
const toMinutes=v=>{const [a,b]=v.split(':').map(Number);return a*60+b;};
function duration(sec){const d=Math.floor(sec/86400),hr=Math.floor(sec%86400/3600),m=Math.floor(sec%3600/60);return d?t('time.dh',{d,h:hr}):hr?t('time.hm',{h:hr,m}):t('time.m',{m});}
function summary(items){return h('dl',{class:'summary'},items.filter(Boolean).map(([k,v,dot])=>h('div',{},h('dt',{i18n:k}),h('dd',{},dot?h('span',{class:'dot '+dot}):null,v))));}
function card(titleKey,leadKey,...body){return h('section',{class:'card'},h('div',{class:'card-head'},h('div',{},h('h2',{i18n:titleKey}),leadKey&&h('p',{class:'help',i18n:leadKey}))),...body);}
function info(key){return h('details',{class:'info'},h('summary',{},icon('info'),h('span',{i18n:'common.more_info'})),h('p',{i18n:key}));}
function pageHead(titleKey,leadKey){return h('div',{},h('h2',{class:'page-title',i18n:titleKey}),h('p',{class:'page-lead',i18n:leadKey}));}
function saveBar(fn,label='common.save'){const b=h('button',{type:'button',class:'btn primary'},h('span',{i18n:label}));b.onclick=()=>run(b,saveDraft(b.closest('[data-settings-page]')||b,fn));return h('div',{class:'form-actions'},b);}
function num(model,key,{min,max,step='any',unit,width}={}){
 const input=h('input',{type:'number',value:model[key],min,max,step,style:width?'width:'+width:null,oninput:e=>{if(e.target.value!==''&&Number.isFinite(+e.target.value))model[key]=+e.target.value;}});
 return unit?h('span',{class:'input-unit'},input,h('em',{},unit)):input;
}
function flag(model,key,titleKey,descKey){const r=switchRow(null,titleKey,descKey);const i=r.querySelector('input');i.checked=!!model[key];i.onchange=()=>{model[key]=typeof model[key]==='boolean'?i.checked:Number(i.checked);};return r;}
function slider(model,key,{min=0,max=100,fmt=v=>v+'%'}={}){
 const out=h('b',{class:'num'},fmt(model[key]));
 const r=h('input',{type:'range',min,max,value:model[key],oninput:e=>{model[key]=+e.target.value;out.textContent=fmt(model[key]);}});
 return [out,r];
}
function normalizeMac(v){const m=(v||'').replace(/[^0-9a-f]/gi,'').toLowerCase();return m.length===12?m:null;}
function prettyMac(m){return (m||'').toUpperCase().replace(/(..)(?!$)/g,'$1:');}
const clone=o=>JSON.parse(JSON.stringify(o));

/* Particulate display bands follow HJ 633-2026 table 3 (not an AQI calculation):
 * https://www.mee.gov.cn/ywgz/fgbz/bz/bzwb/jcffbz/202602/t20260225_1144441.shtml
 * Formaldehyde limit per GB/T 18883-2022,
 * temperature and humidity comfort per the A1 light-segment defaults. */
// CO2 ventilation reference: HSE using-co2-monitors (1000 / 1500 ppm).
const AIR={
 co2:{unit:'ppm',max:2500,bands:[[1000,'comfort','ok'],[1500,'elevated','warn'],[Infinity,'ventilate','bad']]},
 pm25:{unit:'µg/m³',max:250,bands:[[35,'excellent','ok'],[60,'good','ok'],[115,'light','warn'],[150,'moderate','warn'],[250,'heavy','bad'],[Infinity,'severe','bad']]},
 pm10:{unit:'µg/m³',max:420,bands:[[50,'excellent','ok'],[120,'good','ok'],[250,'light','warn'],[350,'moderate','warn'],[420,'heavy','bad'],[Infinity,'severe','bad']]},
 // Product ventilation reference, independent of custom indicator rules; not a health limit.
 // MOX outputs are semiquantitative: https://sensirion.com/resource/application_note/output_mox_sensor
 tvoc:{unit:'ppb',max:1000,bands:[[300,'reference','ok'],[Infinity,'ventilate','warn']]},
 formaldehyde:{unit:'mg/m³',max:0.2,bands:[[0.08,'pass','ok'],[0.1,'slight','warn'],[Infinity,'exceed','bad']]},
 temperature:{unit:'℃',min:0,max:40,bands:[[18,'cool','warn'],[26,'comfort','ok'],[Infinity,'warm','warn']]},
 humidity:{unit:'%',max:100,bands:[[40,'dry','warn'],[60,'comfort','ok'],[Infinity,'humid','warn']]}
};
function grade(metric,v){const b=AIR[metric].bands.find(([lim])=>v<=lim);return {key:'air.'+metric+'.'+b[1],cls:b[2]};}
function airScale(metric,v){
 const m=AIR[metric],lo=m.min||0,span=m.max-lo;let prev=lo;
 const strip=h('div',{class:'scale'});
 for(const [lim,,cls] of m.bands){const end=Math.min(lim,m.max);if(end<=prev)continue;strip.append(h('i',{class:cls,style:'flex:'+((end-prev)/span*100).toFixed(2)}));prev=end;}
 strip.append(h('b',{style:'left:'+Math.max(0,Math.min(100,(v-lo)/span*100))+'%'}));
 return strip;
}
const TZ=[-720,-660,-600,-540,-480,-420,-360,-300,-240,-210,-180,-120,-60,0,60,120,180,210,240,270,300,330,345,360,390,420,480,540,570,600,630,660,720,780,840];
function tzLabel(m){const sg=m<0?'−':'+',a=Math.abs(m);return 'UTC'+sg+String(Math.floor(a/60)).padStart(2,'0')+':'+String(a%60).padStart(2,'0')+(m===480?' · '+t('tz.cn'):'');}

/* ---------- Header, navigation, banners ---------- */
let resetPending=false,resetAccepted=false,resetGeneration=0,resetUptime,resetSawPending=false;
const UI=window.UI={$,h,fieldRow,saveDraft,icon,toast,run,onSubmit,confirm:confirmDialog,switchRow,seg,daysPicker,presetDays,daysText,hhmm,toMinutes,duration,summary,card,info,pageHead,saveBar,num,flag,slider,normalizeMac,prettyMac,clone,AIR,grade,airScale,tzLabel,state:null,caps:null,pages:{},device:null,clockCard,maintenancePage,scanNetworks};
// Keep requested output visible until a fresh read confirms it. Polls may still
// contain the pre-write state while the device applies an accepted command.
let pendingControl=null;
const controlObservers=new Set();
// Partial reads retain slow fields; null explicitly invalidates a measurement.
UI.acceptStatus=(s,partial=false)=>{
 const merged=partial?{...UI.confirmed,...s}:s;
 if(partial&&s.scd40&&UI.confirmed?.scd40)merged.scd40={...UI.confirmed.scd40,...s.scd40};
 UI.confirmed=merged;UI.state=pendingControl?{...merged,...pendingControl}:merged;
 for(const observe of controlObservers)observe(merged);
 return UI.state;
};
function diagnosticSnapshot(reply){
 if(!reply||reply.schema!==diagnosticSchema||!Array.isArray(reply.fields))throw Error('Invalid diagnostics schema');
 const out={},seen=new Set();
 for(const pair of reply.fields){
  if(!Array.isArray(pair)||pair.length!==2||!Number.isInteger(pair[0]))throw Error('Invalid diagnostics field');
  const path=diagnosticFields[String(pair[0])];
  if(!path||pair[1]===undefined)continue;
  if(seen.has(pair[0]))throw Error('Duplicate diagnostics field');
  seen.add(pair[0]);
  const keys=path.split('.');let cursor=out;
  for(const key of keys.slice(0,-1))cursor=cursor[key]||(cursor[key]={});
  cursor[keys.at(-1)]=pair[1];
 }
 return out;
}
function mergeDiagnostic(base,extra){
 const out={...base};
 for(const [key,value] of Object.entries(extra))out[key]=value&&typeof value==='object'&&!Array.isArray(value)?mergeDiagnostic(base[key]&&typeof base[key]==='object'?base[key]:{},value):value;
 return out;
}
let fullGeneration=1,loadedGeneration=0,capabilityGeneration=0,lastFullAt=0;
UI.invalidateStatus=()=>{fullGeneration++;};
UI.activePage=()=>/^settings\/([a-z]+)$/.exec(UI.embeddedRoute??location.hash.slice(1))?.[1]||null;
UI.statusParams=()=>{
 if(UI.caps?.config_recovery||resetPending)return {};
 const page=UI.activePage(),ttl=page==='diagnostics'?5000:['network','mqtt','device','firmware'].includes(page)?10000:30000;
 return UI.caps?.status_views?.includes('live')&&loadedGeneration===fullGeneration&&Date.now()-lastFullAt<ttl?{view:'live'}:{};
};
// Three additional reads at most. Slower application work can still be
// confirmed by ordinary 5-second polling, without extending the read burst.
UI.confirmStatus=matches=>new Promise(resolve=>{
 let settled=false,reads=0,timer;
 const stop=()=>finish(null);
 const finish=value=>{if(settled)return;settled=true;clearTimeout(timer);clearTimeout(deadline);controlObservers.delete(observe);window.removeEventListener('pagehide',stop);resolve(value);};
 const observe=s=>{if(matches(s))finish(s);};controlObservers.add(observe);
 const deadline=setTimeout(stop,12000);window.addEventListener('pagehide',stop,{once:true});
 async function read(){
  await UI.reload({priority:1}).catch(()=>{});
  if(!settled&&++reads<3)timer=setTimeout(read,reads===1?250:750);
 }
 read();
});
UI.control=async(action,params,patch)=>{
 if(!patch||typeof patch!=='object'||Array.isArray(patch)||!Object.keys(patch).length)throw Error(t('error.invalid_params'));
 if(pendingControl){UI.device.onStatus?.(UI.state);throw Error(t('control.pending'));}
 pendingControl=patch;UI.controlBusy=true;
 const render=()=>{if(UI.device.onStatus)UI.device.onStatus(UI.state);};
 UI.state={...UI.state,...patch};render();
 let sendError,last;
 const matches=s=>s&&Object.entries(patch).every(([k,v])=>Object.hasOwn(s,k)&&(Array.isArray(v)?Array.isArray(s[k])&&v.length===s[k].length&&v.every((x,i)=>!!x===!!s[k][i]):typeof v==='boolean'?!!s[k]===v:s[k]===v));
 try{
  try{await command(action,params);}catch(e){sendError=e;if(e.code&&!['timeout','offline'].includes(e.code))throw e;}
  last=await UI.confirmStatus(matches);
  if(last)return last;
  throw Error(t('control.unconfirmed')+(sendError?' · '+sendError.message:''));
 }finally{
  pendingControl=null;UI.controlBusy=false;UI.state=UI.confirmed||UI.state;render();
 }
};
function header(dev){
 const lang=h('select',{id:'langPick','aria-label':'Language'},
  h('option',{value:'auto','data-i18n':'pref.lang_auto'},t('pref.lang_auto')),h('option',{value:'en'},'English'),h('option',{value:'zh-CN'},'简体中文'));
 lang.value=I18N.preference;lang.onchange=()=>I18N.set(lang.value);
 const theme=h('select',{id:'themePick','aria-label':'Theme'},['system','light','dark'].map(v=>h('option',{value:v,'data-i18n':'pref.theme_'+v},t('pref.theme_'+v))));
 let saved='system';try{saved=localStorage.getItem('phicomm.appearance')||'system';}catch{}
 const apply=v=>{if(v==='system')document.documentElement.removeAttribute('data-theme');else document.documentElement.dataset.theme=v;};
 theme.value=saved;apply(saved);theme.onchange=()=>{apply(theme.value);try{localStorage.setItem('phicomm.appearance',theme.value);}catch{}};
 return h('header',{class:'topbar'},
  h('div',{class:'brand'},h('div',{class:'logo'},icon(dev.icon)),h('div',{class:'grow'},h('div',{class:'kind','data-i18n':dev.kindKey},t(dev.kindKey)),h('h1',{id:'title'},dev.model))),
  h('div',{class:'prefs'},lang,theme,globalThis.DeviceSession&&h('button',{type:'button',class:'btn',onclick:()=>DeviceSession.logout().catch(e=>toast(e.message,true))},I18N.lang==='en'?'Sign out':'退出登录')),
  h('div',{class:'chips',id:'chips'}));
}
function chip(dotCls,labelKey,value){return h('span',{class:'chip'},h('span',{class:'dot '+dotCls}),h('span',{i18n:labelKey}),value!=null&&h('b',{},value));}
function renderChips(s){
 const c=$('chips');if(!c)return;
 const wifi=s.wifi_connected?chip('ok','chip.wifi',s.ssid+' · '+s.ip):s.ap_active?chip('warn','chip.hotspot',s.ap_name):chip('bad','chip.wifi',t('state.connecting'));
 const mq=!s.mqtt_host?chip('','chip.mqtt',t('state.off')):s.mqtt_connected?chip('ok','chip.mqtt',t('state.ready')):chip('warn','chip.mqtt',t('s.'+(s.mqtt_status||'connecting')));
 c.replaceChildren(wifi,mq,h('span',{class:'chip'},h('span',{i18n:'chip.firmware'}),h('b',{class:'num'},s.version||'—')),...(mayM1&&UI.device.model==='M1'?[h('span',{class:'chip'},h('span',{i18n:'mcu.version'}),h('b',{class:'num'},mcuVersion?.version_received?mcuVersion.version:t('mcu.unknown')))]:[]),...(UI.device.chips?UI.device.chips(s):[]));
}
function renderBanners(s,lost){
 const b=$('banners');if(!b)return;b.replaceChildren();
 const add=(cls,ic,text)=>b.append(h('div',{class:'banner '+cls,role:cls==='bad'?'alert':null},icon(ic),h('span',{},text)));
 if(lost){add('bad','alert',t('banner.lost'));return;}
 if(UI.caps?.config_recovery)add('warn','alert',t('recovery.hint'));
 if(resetPending)add('info','auto',t('toast.resetting'));
 if(s.ota_active)add('info','auto',t('banner.ota',{p:s.ota_progress}));
 if(s.reboot_pending)add('info','auto',t('banner.rebooting'));
 if(s.mqtt_cleanup_warning)add('warn','alert',t('banner.cleanup'));
 if(s.save_error)add('bad','alert',t('banner.save_error',{e:s.save_error}));
 if(s.ap_active&&!s.wifi_connected)add('warn','wifi',t('banner.hotspot',{n:s.ap_name}));
}
function buildNav(dev){
 const side=$('side');side.replaceChildren();
 for(const [groupKey,keys] of dev.groups){
  const links=keys.filter(k=>UI.pages[k]).map(k=>h('a',{href:'#settings/'+k,'data-i18n':dev.nav[k]},t(dev.nav[k])));
  if(!links.length)continue;
  side.append(h('div',{class:'group','data-i18n':groupKey},t(groupKey)));
  for(const a of links)side.append(a);
 }
}
let routeGeneration=0;let routeRead=null;
function showRoute(){
 const generation=++routeGeneration;
 routeRead?.abort();routeRead=new AbortController();
 const m=/^settings\/([a-z]+)$/.exec(UI.embeddedRoute??location.hash.slice(1));
 const page=m&&document.querySelector('[data-settings-page="'+m[1]+'"]')?m[1]:m?(UI.device.groups.flatMap(g=>g[1]).find(k=>UI.pages[k])||null):null;
 $('homePage').hidden=!!page;$('settingsPage').hidden=!page;
 document.querySelectorAll('[data-settings-page]').forEach(p=>{p.hidden=p.dataset.settingsPage!==page;});
 document.querySelectorAll('.tabs a').forEach(a=>{const on=a.dataset.tab===(page?'settings':'home');if(on)a.setAttribute('aria-current','page');else a.removeAttribute('aria-current');});
 document.querySelectorAll('.side a').forEach(a=>{if(a.hash==='#settings/'+page)a.setAttribute('aria-current','page');else a.removeAttribute('aria-current');});
 // A slow status refresh must not launch reads for tabs already left behind.
 if(page)UI.reload({full:true}).then(()=>{if(generation===routeGeneration)afterShow(page);});else afterShow(page);
}
const showingPages=new Map();
async function afterShow(page){
 if(!page)return;
 const active=document.querySelector('.side a[aria-current]'),side=$('side');
 // Reveal mobile menu items without scrollIntoView scrolling the whole page.
 if(active&&side&&side.scrollWidth>side.clientWidth){
  const item=active.getBoundingClientRect(),menu=side.getBoundingClientRect();
  if(item.left<menu.left)side.scrollLeft+=item.left-menu.left;
  else if(item.right>menu.right)side.scrollLeft+=item.right-menu.right;
 }
 const p=UI.pages[page],signal=routeRead?.signal;if(!p?.show||signal?.aborted||showingPages.get(page)===signal)return;
 const box=document.querySelector('[data-settings-page="'+page+'"]');
 if(box?.matches('[data-dirty="1"]')||box?.querySelector('[data-dirty="1"]'))return;
 box?.querySelector('.page-load-error')?.remove();showingPages.set(page,signal);
 if(['light','linkage','freshair','night'].includes(page))box.dataset.preserveDraft='1';
 const edit=event=>{const draft=event.target.closest('[data-preserve-draft]');if(!draft)return;box.dataset.editVersion=String(1+Number(box.dataset.editVersion||0));draft.dataset.dirty='1';};
 if(box&&!box.dataset.trackEdits){box.dataset.trackEdits='1';box.addEventListener('input',edit,true);box.addEventListener('change',edit,true);box.addEventListener('click',event=>{if(event.target.closest('.seg button'))edit(event);},true);}
 const version=box?.dataset.editVersion;
 try{await p.show({signal,current:()=>!signal?.aborted&&box?.dataset.editVersion===version});}catch(e){if(signal?.aborted||e.name==='AbortError')return;const retry=h('button',{class:'btn',type:'button',onclick:()=>afterShow(page)},t('common.retry'));box?.prepend(h('div',{class:'banner warn page-load-error',role:'alert'},h('span',{},e.message),retry));}finally{if(showingPages.get(page)===signal)showingPages.delete(page);}
}
/* Wi-Fi scan is shared: start, poll, and hand the sorted list to a callback. */
async function scanNetworks(onList){
 await command('wifi.scan');
 for(let i=0;i<25;i++){const r=await command('wifi.scan.get');if(r.scanning){await new Promise(res=>setTimeout(res,700));continue;}onList(r.networks||[]);return;}
 throw Error(t('wifi.scan_timeout'));
}

/* ---------- Shared settings pages ---------- */
function wifiPage(){
 const sum=h('div'),list=h('div',{class:'list',hidden:true});
 const ssid=h('input',{id:'ssid',maxLength:32,required:true,autocomplete:'off'});
 const pass=h('input',{id:'wifiPassword',type:'password',maxLength:63,autocomplete:'new-password',placeholder:t('wifi.pass_ph')});
 const open=switchRow('openNetwork','wifi.open');
 const scanBtn=h('button',{type:'button',class:'btn',id:'scan'},icon('auto'),h('span',{i18n:'wifi.scan'}));
 const form=h('form',{id:'wifiForm',class:'stack'},
  fieldRow('wifi.ssid',h('div',{class:'row'},h('div',{class:'grow'},ssid),scanBtn)),list,
  fieldRow('wifi.password',pass),open,
  h('div',{class:'form-actions'},h('button',{class:'btn primary',type:'submit'},h('span',{i18n:'wifi.save'}))),info('wifi.hint'));
 open.querySelector('input').onchange=e=>{pass.disabled=e.target.checked;};
 scanBtn.onclick=()=>run(scanBtn,()=>scanNetworks(net=>{
  list.replaceChildren(...net.slice().sort((a,b)=>b.rssi-a.rssi).map(n=>h('button',{type:'button',class:'item',style:'cursor:pointer;text-align:left;font:inherit;color:inherit;width:100%',
   onclick:()=>{ssid.value=n.ssid;open.querySelector('input').checked=n.security===0;pass.disabled=n.security===0;list.hidden=true;pass.focus();}},
   icon('wifi'),h('span',{class:'grow'},h('b',{},n.ssid),h('span',{},t('wifi.signal.'+(n.rssi>-55?3:n.rssi>-70?2:1))+' · '+n.rssi+' dBm')),n.security===0?h('span',{class:'badge',i18n:'wifi.open_badge'}):icon('lock'))));
  list.hidden=false;
  if(!net.length)throw Error(t('wifi.none'));
 }),null);
 onSubmit(form,()=>api('/wifi/config',{ssid:ssid.value,password:pass.value,open_network:open.querySelector('input').checked}),'wifi.verifying');
 return {el:h('div',{class:'stack'},pageHead('nav.wifi','wifi.lead'),sum,card('wifi.change',null,form)),
  status(s){sum.replaceChildren(summary([['wifi.status',s.wifi_connected?t('state.connected'):s.ap_active?t('state.hotspot'):t('state.connecting'),s.wifi_connected?'ok':s.ap_active?'warn':'bad'],
   ['wifi.ssid',s.ssid||'—'],['wifi.ip',s.ip||'—'],s.ap_active?['wifi.ap',s.ap_name]:['wifi.saved_password',t(s.wifi_password_set?'common.yes':'common.no')]]));},
  init(s){ssid.value=s.ssid||'';}};
}
function mqttImportPanel(dev,fields){
 const box=h('section',{class:'card stack',id:'mqttImport',hidden:!importTicket&&!importInvalid});
 if(importInvalid){box.append(h('p',{role:'alert'},t('import.invalid')));return box;}
 if(!importTicket)return box;
 const ticket=importTicket;importTicket=null;
 const source=window.opener;
 const status=h('p',{role:'status'}),accept=h('button',{type:'button',class:'btn primary',disabled:true},t('import.accept')),
 cancel=h('button',{type:'button',class:'btn'},t('common.cancel'));
 box.append(h('h2',{},t('import.title')),h('p',{},t('import.source',{origin:ticket.origin})),h('p',{},t('import.preview',{host:ticket.host,port:ticket.port,user:ticket.user||'—'})),
  h('p',{class:'help'},t('import.help')),h('p',{class:'help'},t('import.http')),
  h('div',{class:'form-actions'},accept,cancel),status);
 let waiting=false,done=false,timer,original=null;
 box.activate=()=>{if(!done&&source&&Date.now()<ticket.expires)accept.disabled=false;};
 const detach=()=>{window.removeEventListener('message',receive);clearTimeout(timer);try{window.opener=null;}catch{}};
 const clear=()=>{fields.pass.value='';if(original){fields.host.value=original.host;fields.port.value=original.port;fields.user.value=original.user;if(fields.protocol)fields.protocol.value=original.protocol;fields.clear.checked=original.clear;original=null;}waiting=false;done=true;detach();accept.disabled=true;};
 box.saved=()=>{original=null;fields.pass.value='';clearTimeout(timer);};
 box.guard=()=>{if(waiting)throw Error(t('import.waiting'));};
 const finish=()=>{clear();status.textContent=t('import.expired');};
 const receive=e=>{
  if(!waiting||done||!IotProvision.packet(e,{source,origin:ticket.origin,id:ticket.id,expires:ticket.expires},'iotcloud.provision.config'))return;
  let data;
  try{if(Object.keys(e.data).sort().join(',')!=='id,password,type')throw Error();data=IotProvision.config({v:1,host:ticket.host,port:ticket.port,user:ticket.user,password:e.data.password,protocol:'tmcp',expires:ticket.expires});}catch{finish();status.textContent=t('import.invalid');return;}
  // Only front-end fields are touched. config.set remains the normal save flow.
  fields.host.value=data.host;fields.port.value=String(data.port);fields.user.value=data.user;fields.pass.value=data.password;
  if(fields.protocol)fields.protocol.value='tmcp';fields.clear.checked=data.password==='';
  waiting=false;done=true;detach();accept.hidden=true;status.textContent=t('import.received');
  source.postMessage({type:'iotcloud.provision.received',id:ticket.id},ticket.origin);
  // Never leave an unsaved imported host paired with the device's old password.
  timer=setTimeout(()=>{clear();status.textContent=t('import.expired');},Math.max(0,ticket.expires-Date.now()));
 };
 cancel.onclick=()=>{clear();box.hidden=true;};
 accept.onclick=()=>{
  if(done||Date.now()>=ticket.expires||!source){finish();return;}
  original={host:fields.host.value,port:fields.port.value,user:fields.user.value,protocol:fields.protocol?.value,clear:fields.clear.checked};
  fields.host.value=ticket.host;fields.port.value=String(ticket.port);fields.user.value=ticket.user;fields.pass.value='';if(fields.protocol)fields.protocol.value='tmcp';fields.clear.checked=false;
  waiting=true;accept.disabled=true;status.textContent=t('import.waiting');
  source.postMessage({type:'iotcloud.provision.ready',id:ticket.id},ticket.origin);
 };
 if(!source){accept.disabled=true;status.textContent=t('import.no_opener');return box;}
 window.addEventListener('message',receive);
 timer=setTimeout(finish,Math.max(0,ticket.expires-Date.now()));
 window.addEventListener('pagehide',clear,{once:true});
 window.addEventListener('hashchange',()=>{if(location.hash!=='#settings/mqtt')clear();});
 return box;
}
function mqttPage(dev){
 const sum=h('div');
 const host=h('input',{id:'mqttHost',maxLength:dev.mqttHostMax||127,placeholder:t('mqtt.host_ph')});
 const port=h('input',{id:'mqttPort',type:'number',min:1,max:65535,required:true,value:'1883'});
 const user=h('input',{id:'mqttUser',maxLength:63,autocomplete:'off'});
 const pass=h('input',{id:'mqttPassword',type:'password',maxLength:dev.mqttPasswordMax||256,autocomplete:'new-password',placeholder:t('mqtt.pass_ph')});
 const protocol=(PRODUCT===null||PRODUCT==='tc1')&&dev.model==='TC1'?h('select',{id:'mqttMode'},h('option',{value:'tmcp'},'TMCP (v2)'),h('option',{value:'legacy'},'Legacy (v1)')):null;
 const clear=switchRow('clearMqttPassword','mqtt.clear','mqtt.clear_desc');
 const imported=mqttImportPanel(dev,{host,port,user,pass,protocol,clear:clear.querySelector('input')});
 const form=h('form',{id:'mqttForm',class:'stack'},
  h('div',{class:'fields',style:'grid-template-columns:minmax(0,3fr) minmax(90px,1fr)'},
   fieldRow('mqtt.host',host),fieldRow('mqtt.port',port)),
  h('div',{class:'fields'},fieldRow('mqtt.user',user),fieldRow('mqtt.password',pass)),
  h('details',{class:'more'},h('summary',{i18n:'common.advanced'}),...(protocol?[fieldRow('mqtt.mode',protocol)]:[]),clear),info(dev.haHintKey),
  h('div',{class:'form-actions'},h('button',{class:'btn primary',type:'submit'},h('span',{i18n:'common.save'}))));
 onSubmit(form,()=>{
  imported.guard?.();
  const enc=new TextEncoder();if(enc.encode(user.value).length>63||enc.encode(pass.value).length>256)throw Error(t('mqtt.too_long'));
  return api('/mqtt/config',{host:host.value.trim(),port:Number(port.value),user:user.value,password:pass.value,clear_password:clear.querySelector('input').checked,...(protocol?{protocol:protocol.value}:{})}).then(result=>{pass.value='';imported.saved?.();return result;});
 });
 const broker=card('mqtt.broker',null,form);
 const heading=broker.querySelector('.card-head');heading.classList.add('broker-card-head');
 const hub=!UI.embedded&&UI.caps?.model==='daikin'&&UI.caps.firmware_variant==='hub';
 heading.append(h('a',{class:'btn broker-platform-link',href:hub?'/iotcloud/#/':'https://phicomm-io.github.io/iotcloud/#/',target:hub?null:'_blank',rel:hub?null:'noopener noreferrer'},h('span',{i18n:hub?'nav.hub':'import.platform'}),icon('external')));
 return {el:h('div',{class:'stack'},pageHead('nav.mqtt','mqtt.lead'),sum,imported,broker),
  status(s){sum.replaceChildren(summary([['mqtt.status',!s.mqtt_host?t('state.off'):s.mqtt_connected?t('state.ready'):t('s.'+(s.mqtt_status||'connecting')),!s.mqtt_host?'':s.mqtt_connected?'ok':'warn'],
   ['mqtt.host',s.mqtt_host?s.mqtt_host+':'+s.mqtt_port:'—'],['mqtt.mode',t(s.mqtt_topic_mode==='legacy'?'mqtt.mode_legacy':'mqtt.mode_tmcp')],['mqtt.user',s.mqtt_user||'—']]));},
  init(s){if(protocol)protocol.value=s.mqtt_protocol||s.mqtt_topic_mode||'tmcp';host.value=s.mqtt_host||'';port.value=s.mqtt_port||1883;user.value=s.mqtt_user||'';imported.activate?.();}};
}
let mcuVersion=null,mcuReadAt=0,mcuReading=null;
function mcuFamily(data){
 if(!data?.version_received||!data.version)return 'unknown';
 if(/^17\./.test(data.version))return 'stock17';
 if(/^11\./.test(data.version))return 'stock11';
 if(/^v?\d+\.\d+\.\d+(?:[-+].*)?$/.test(data.version)&&data.extension_protocol)return 'open';
 return 'unverified';
}
async function readMcuVersion(force=false){
 if(UI.device?.model!=='M1'||!UI.caps?.actions?.includes('mcu.version.get'))return null;
 if(mcuReading)return mcuReading;
 if(!force&&Date.now()-mcuReadAt<15000)return mcuVersion;
 mcuReadAt=Date.now();
 mcuReading=command('mcu.version.get').then(data=>{mcuVersion=data;renderChips(UI.state||{});return data;}).finally(()=>{mcuReading=null;});
 return mcuReading;
}
function mcuVersionPanel(){
 const value=h('b',{class:'num'}),hint=h('p',{class:'help'});
 const button=h('button',{class:'btn',type:'button'},h('span',{i18n:'mcu.query'}));
 const panel=card('mcu.version',null,value,hint,button);
 function render(){value.textContent=mcuVersion?.version_received?mcuVersion.version:t('mcu.unknown');const family=mcuFamily(mcuVersion);hint.textContent=t('mcu.'+family);hint.className='help'+(family==='stock17'?' warn':'');}
 button.onclick=()=>run(button,async()=>{await command('mcu.version.query');await new Promise(resolve=>setTimeout(resolve,800));await readMcuVersion(true);render();});
 I18N.onChange(render);render();
 return {el:panel,async refresh(){try{await readMcuVersion();}finally{render();}}};
}
let releaseNotesLoading=null;
function loadReleaseNotes(){
 if(globalThis.ReleaseNotes&&globalThis.ReleaseNotesClient)return Promise.resolve();
 if(releaseNotesLoading)return releaseNotesLoading;
 const load=name=>new Promise((resolve,reject)=>{const script=document.createElement('script'),finish=error=>{clearTimeout(timer);script.onload=script.onerror=null;if(error){script.remove();reject(error);}else resolve();},timer=setTimeout(()=>finish(Error('Timeout')),7000);script.src='https://phicomm-io.github.io/release-notes/'+name+'.js';script.onload=()=>finish();script.onerror=()=>finish(Error('Network'));document.head.append(script);});
 releaseNotesLoading=load('core').then(()=>load('client')).catch(error=>{releaseNotesLoading=null;throw error;});return releaseNotesLoading;
}
function downloadFile(data,name,type='application/octet-stream'){
 const url=URL.createObjectURL(new Blob([data],{type})),a=h('a',{href:url,download:name});document.body.append(a);a.click();a.remove();setTimeout(()=>URL.revokeObjectURL(url),1000);
}
function backupCard(){
 const full=!!UI.caps?.config_backup;
 const download=h('button',{class:'btn',type:'button'},h('span',{i18n:full?'backup.download':'backup.public'}));
 download.onclick=()=>run(download,async()=>{
  if(!full){downloadFile(JSON.stringify({schema:1,partial:true,model:UI.caps.model,config:await command('config.get')},null,2),'phicomm-public-settings.json','application/json');return;}
  const response=await deviceFetch('/api/v1/config/backup',{cache:'no-store',credentials:'same-origin'});if(!response.ok)throw Error(t('backup.failed'));
  downloadFile(await response.arrayBuffer(),'phicomm-'+String(UI.caps.model||'device').toLowerCase()+'-config.phcb');
 });
 const file=h('input',{type:'file',accept:'.phcb,.json',required:true});
 const form=h('form',{class:'stack',hidden:!full},file,h('button',{type:'submit',class:'btn'},h('span',{i18n:'backup.restore'})));
 onSubmit(form,async()=>{
  if(!file.files?.[0]||file.files[0].size>16384)throw Error(t('backup.failed'));
  if(!await confirmDialog({title:t('backup.restore'),body:t('backup.confirm'),ok:t('backup.restore'),danger:true}))return;
  const response=await deviceFetch('/api/v1/config/restore',{method:'POST',headers:{'Content-Type':'application/octet-stream'},body:await file.files[0].arrayBuffer(),credentials:'same-origin'});
  if(response.status!==202)throw Error(t('backup.failed'));toast(t('backup.restoring'));
 });
 return card('backup.title',null,h('p',{class:'help',i18n:full?'backup.hint':'backup.partial'}),download,form);
}
function firmwarePage(dev){
 const mcu=mayM1&&dev.model==='M1'?mcuVersionPanel():null;
 const comparison=h('a',{id:'otaCompare',class:'btn',target:'_blank',rel:'noopener noreferrer'},h('span',{i18n:'fw.compare'}),icon('external'));
 let identity=null,targetVersion=null;
 function comparisonLink(){
  const path=I18N.lang==='en'?'guides/en/release-log.html':'guides/release-log.html';
  const link=new URL(path,'https://phicomm-io.github.io/');link.searchParams.set('view','compare');
  const model=identity?.model||UI.caps?.model,variant=identity?.firmware_variant||UI.caps?.firmware_variant;
  if(model&&variant&&identity?.version){
   for(const [key,value] of Object.entries({model,variant,from:identity.version,...(targetVersion?{to:targetVersion}:{})}))link.searchParams.set(key,value);
  }
  comparison.href=link.href;
 }
 I18N.onChange(comparisonLink);comparisonLink();
 const online=h('section',{class:'card stack',hidden:!UI.caps?.actions?.includes('ota.sources.get')});
 if(!online.hidden){
  const sources=h('div',{class:'row',id:'otaSources'}),latest=h('p',{class:'help'});
  const notes=h('button',{class:'btn',type:'button',hidden:true},t('fw.notes'));
  const notesText=h('pre',{class:'help',hidden:true,style:'white-space:pre-wrap;max-height:24rem;overflow:auto'});
  const install=h('button',{class:'btn primary',type:'button',hidden:true},t('fw.install_online'));
  let timer=null,last=null;
  const buttons=[];
  function busy(value){for(const button of buttons)button.disabled=value;}
  function translate(){install.textContent=t('fw.install_online');notes.textContent=t('fw.notes');for(const button of buttons)button.textContent=t('fw.source_'+button.dataset.source);if(last)latest.textContent=t('fw.check_'+last.state)+(last.version?' · '+last.version:'');}
  I18N.onChange(translate);
  async function poll(){
   const data=await command('ota.check.get');last=data;
   targetVersion=['available','up_to_date'].includes(data.state)?data.version:null;comparisonLink();
   latest.textContent=t('fw.check_'+data.state)+(data.version?' · '+data.version:'')+(data.error?' · '+data.error:'');
   install.hidden=!data.version||data.state==='failed';notes.hidden=!targetVersion;
   if(data.busy||data.state==='checking')timer=setTimeout(()=>poll().catch(e=>{latest.textContent=e.message;busy(false);}),1000);
   else busy(false);
  }
  async function check(source){
   if(timer)clearTimeout(timer);busy(true);install.hidden=notes.hidden=notesText.hidden=true;targetVersion=null;comparisonLink();
   try{await command('ota.check',{source});latest.textContent=t('fw.check_checking');timer=setTimeout(()=>poll().catch(e=>{latest.textContent=e.message;busy(false);}),500);}
   catch(e){busy(false);throw e;}
  }
  let comparisonReady='';
  async function readComparison(){
   const key=[identity?.version,targetVersion,I18N.lang].join('/');
   if(comparisonReady===key)return true;
   notesText.hidden=false;
   try{
    if(!identity?.version||!targetVersion)throw Error();
    await loadReleaseNotes();
    const snapshot=await ReleaseNotesClient.snapshot('https://phicomm-io.github.io');
    const result=ReleaseNotes.compare(snapshot.manifest,{from:identity.version,to:targetVersion,model:String(identity.model||UI.caps.model).toLowerCase(),variant:identity.firmware_variant||UI.caps.firmware_variant||'full',locale:I18N.lang},snapshot.catalog);
    if(!result.ok||key!==[identity?.version,targetVersion,I18N.lang].join('/'))throw Error();
    notesText.textContent=[...result.upgrade_notices,...result.changes].map(row=>row.title+'\n'+row.description).join('\n\n')||t('fw.check_up_to_date');comparisonReady=key;return true;
   }catch{comparisonReady='';notesText.textContent=t('fw.notes_unavailable');return false;}
  }
  notes.onclick=()=>run(notes,readComparison);
  install.onclick=()=>run(install,async()=>{
   if(!await readComparison()&&!window.confirm(t('fw.notes_confirm')))return;
   if(!window.confirm(t('fw.confirm_online')))return;await command('ota.install');toast(t('fw.url_started'));
  });
  command('ota.sources.get').then(data=>{for(const id of data.sources){const button=h('button',{class:'btn',type:'button','data-source':id},t('fw.source_'+id));button.onclick=()=>run(button,()=>check(id));buttons.push(button);sources.append(button);}}).catch(()=>{online.hidden=true;});
  online.append(h('h2',{i18n:'fw.online'}),h('p',{class:'help',i18n:'fw.sources_help'}),sources,latest,h('div',{class:'row'},install,notes),notesText);
 }
 const cancel=h('button',{class:'btn',type:'button',hidden:true,i18n:'fw.cancel'});
 cancel.onclick=()=>run(cancel,async()=>{await command('ota.cancel');toast(t('fw.cancelling'));});
 const sum=h('div'),prog=h('progress',{max:100,value:0}),state=h('p',{class:'help'});
 const file=h('input',{id:'firmware',type:'file',accept:'.bin,.raw,.signed',required:true});
 const unsigned=switchRow('unsignedRescue','fw.unsigned','fw.unsigned_desc');
 const conversionSupported=UI.caps?.ota_variant_conversion==='full-to-hub';
 const conversion=conversionSupported?switchRow('hubConversion','fw.convert_hub','fw.convert_hub_help'):null;
 const conversionInput=conversion?.querySelector('input'),unsignedInput=unsigned.querySelector('input');
 function conversionMode(){
  const selected=UI.caps?.ota_variant_conversion==='full-to-hub'&&!!conversionInput?.checked;
  if(selected)unsignedInput.checked=false;
  unsignedInput.disabled=selected;
  file.accept=selected?'.signed':'.bin,.raw,.signed';
 }
 conversionInput?.addEventListener('change',conversionMode);
 const url=h('input',{id:'otaUrl',type:'url',maxLength:UI.caps?.ota_url_max||499,required:true,placeholder:'https://…/'+dev.otaName});
 const fileForm=h('form',{id:'fileForm',class:'stack'},h('label',{class:'field'},h('span',{},t('fw.file',{m:dev.model})),file),
  h('details',{class:'more'},h('summary',{i18n:'common.advanced'}),unsigned,...(conversion?[conversion]:[])),
  h('div',{class:'form-actions'},h('button',{class:'btn primary',type:'submit'},h('span',{i18n:'fw.upload'}))));
 const urlForm=h('form',{id:'urlForm',class:'stack',hidden:true},fieldRow('fw.url',url),
  h('div',{class:'form-actions'},h('button',{class:'btn primary',type:'submit'},h('span',{i18n:'fw.download'}))));
 const picker=seg([['file','fw.tab_file'],['url','fw.tab_url']],v=>{fileForm.hidden=v!=='file';urlForm.hidden=v!=='url';});
 function installCapabilities(caps){
  const urlOnly=caps?.ota_upload===false;
  picker.set(urlOnly?'url':'file');
  fileForm.hidden=urlOnly;urlForm.hidden=!urlOnly;
  picker.hidden=urlOnly||!caps?.actions?.includes('ota.start');
 }
 installCapabilities(UI.caps);
 // Upload streams raw bytes; the request is synchronous up to send(), like the old page.
 let uploading=false;
 fileForm.addEventListener('submit',e=>{
  e.preventDefault();if(UI.identityChanged){toast(I18N.lang==='en'?'Refresh this page before uploading.':'请刷新页面后再上传。',true);return;}const f=file.files&&file.files[0];if(!f||uploading)return;
  const rescue=unsignedInput.checked,convert=!!conversionInput?.checked;
  if(convert&&UI.caps?.ota_variant_conversion!=='full-to-hub'){toast(t('error.unsupported'),true);return;}
  if(convert){
   if(rescue||!f.name?.endsWith('.signed')){toast(t('fw.convert_hub_signed'),true);return;}
   if(!window.confirm(t('fw.convert_hub_confirm')))return;
  }
  if(rescue&&!window.confirm(t('fw.unsigned_confirm')))return;
  uploading=true;prog.value=0;
  const xhr=window.createDeviceUpload?window.createDeviceUpload():new XMLHttpRequest();xhr.open('POST',convert?'/api/v1/ota/convert':rescue?'/api/v1/ota/upload-unsigned':'/api/v1/ota/upload');
  const authorization=globalThis.DeviceSession?.headers().Authorization;xhr.setRequestHeader('Content-Type','application/octet-stream');for(const [name,value] of Object.entries(globalThis.DeviceSession?.headers()||{}))xhr.setRequestHeader(name,value);xhr.timeout=310000;if(globalThis.DeviceSession){xhr.responseType='arraybuffer';xhr.setRequestHeader('Accept',DeviceSession.wire.headers().Accept);}
  if(convert){xhr.setRequestHeader('X-IoTCloud-Target-Variant','hub');xhr.setRequestHeader('X-IoTCloud-Confirm','full-to-hub');}
  xhr.upload.onprogress=ev=>{if(ev.lengthComputable){prog.value=ev.loaded/ev.total*100;state.textContent=t('fw.uploading',{p:Math.round(ev.loaded/ev.total*100)});}};
  xhr.onload=async()=>{
   globalThis.DeviceSession?.observe(xhr.status,authorization);const ok=xhr.status===202;let detail='';
   if(!ok)try{if(globalThis.DeviceSession&&xhr.response!==undefined&&xhr.response!==null){const reply=await DeviceSession.wire.decodeResponse(new Response(xhr.response,{status:xhr.status,headers:{'Content-Type':xhr.getResponseHeader?.('Content-Type')||DeviceSession.wire.headers().Accept}}));detail=reply.message||reply.code||'';}else detail=xhr.responseText||'';}catch{detail='HTTP '+xhr.status;}
   state.textContent=ok?t('fw.verify_ok'):t('fw.failed',{e:detail});toast(state.textContent,!ok);
  };
  xhr.onerror=()=>toast(t('fw.upload_lost'),true);
  xhr.ontimeout=()=>toast(t('fw.upload_timeout'),true);
  xhr.onloadend=()=>{uploading=false;};
  xhr.send(f);
 });
 onSubmit(urlForm,()=>api('/ota',{url:url.value}),'fw.url_started');
 const fileLabel=fileForm.querySelector('.field>span');I18N.onChange(()=>{fileLabel.textContent=t('fw.file',{m:dev.model});});
 return {el:h('div',{class:'stack'},pageHead('nav.firmware','fw.lead'),sum,...(mcu?[mcu.el]:[]),card('fw.compare_title',null,h('p',{class:'help',i18n:'fw.compare_help'}),comparison),online,
  card('fw.install',null,picker,h('div',{style:'margin-top:16px'},fileForm,urlForm),h('div',{class:'stack',style:'margin-top:20px'},prog,state,cancel),info(dev.otaHintKey))),
  capabilities(caps){if(conversion&&caps.ota_variant_conversion!=='full-to-hub'){conversionInput.checked=false;conversion.remove();conversionMode();}online.hidden=!caps.actions?.includes('ota.sources.get');installCapabilities(caps);},
  status(s){identity=s;comparisonLink();if(mcu)mcu.refresh().catch(()=>{});sum.replaceChildren(summary([['fw.version',h('span',{class:'num'},s.version||'—')],['fw.state',t('s.'+(s.ota_state||'idle'))],s.ota_error?['fw.error',h('span',{class:'badge bad'},s.ota_error)]:null]));
   cancel.hidden=!UI.caps?.actions?.includes('ota.cancel')||!s.ota_active||s.ota_mode==='committing';
   if(s.ota_active){prog.value=s.ota_progress;state.textContent=t('fw.progress',{p:s.ota_progress});}}};
}
function diagValue(k,v){
 if(v===null||v===undefined||v==='')return '—';
 if(typeof v==='boolean')return t(v?'common.yes':'common.no');
 if(k==='uptime_seconds')return duration(v);
 if(k==='free_memory')return (v/1024).toFixed(1)+' KB';
 if(k==='age_ms')return (v/1000).toFixed(1)+' s';
 if(k==='ntp_last_success'||k==='utc_time')return new Date(v*1000).toLocaleString(I18N.lang);
 if(Array.isArray(v))return v.join(', ');
 if(typeof v==='string'&&I18N.has('s.'+v))return t('s.'+v);
 return String(v);
}
function diagnosticsPage(dev){
 const health=h('div',{class:'health'}),groups=h('div',{class:'diag'}),raw=h('pre'),updated=h('p',{class:'help'});
 const extra=dev.diagExtra?dev.diagExtra():null;
 return {el:h('div',{class:'stack'},pageHead('nav.diagnostics','diag.lead'),card('diag.health',null,health),
   h('section',{},h('div',{class:'section-label',i18n:'diag.details'}),groups),extra,
   h('details',{class:'more'},h('summary',{i18n:'diag.raw'}),raw),updated),
  status(s){
   if(!UI.pages.diagnostics||$('settingsPage').hidden)return;
   const checks=[['diag.c_wifi',s.wifi_connected?'ok':'bad',s.wifi_connected?s.ssid:t('state.disconnected')],
    ['diag.c_mqtt',!s.mqtt_host?'':s.mqtt_connected?'ok':'warn',!s.mqtt_host?t('state.off'):t('s.'+(s.mqtt_status||'connecting'))+(s.mqtt_errors?' · '+t('diag.errors',{n:s.mqtt_errors}):'')],
    ...(Object.hasOwn(s,'watchdog_state')?[['diag.c_watchdog',!s.watchdog_enabled||s.watchdog_state==='fault'?'bad':s.reset_reason==='watchdog'?'warn':'ok',s.reset_reason==='watchdog'?t('diag.watchdog_reboot'):t('s.'+s.watchdog_state)]]:[]),
    ...(dev.healthChecks?dev.healthChecks(s):[]),['diag.c_ota',s.ota_error?'bad':'ok',s.ota_error||t('s.'+(s.ota_state||'idle'))]];
   health.replaceChildren(...checks.map(([k,cls,text])=>h('div',{class:'item'},h('span',{class:'dot '+cls}),h('span',{class:'grow'},h('b',{i18n:k})),h('span',{class:'help'},text))));
   const seen=new Set();
   const all=[['diag.g_device',['name','device_id','version','uptime_seconds','free_memory','web_password_set']],
    ['diag.g_watchdog',['reset_reason','reset_reason_raw','watchdog_enabled','watchdog_state','watchdog_fault']],
    ['diag.g_network',['wifi_connected','ssid','ip','ap_active','ap_name','scan_busy','wifi_password_set','wifi_trial_status','wifi_trial_active']],
    ['diag.g_mqtt',['mqtt_connected','mqtt_status','mqtt_host','mqtt_port','mqtt_user','mqtt_topic_mode','mqtt_password_set','mqtt_errors','mqtt_last_error','mqtt_cleanup_warning']],
    ...(dev.diagGroups||[]),['diag.g_ota',['ota_active','ota_state','ota_progress','ota_error']]];
   const open=new Set([...groups.querySelectorAll('details')].filter(d=>d.open).map(d=>d.dataset.key));
   groups.replaceChildren(...all.map(([key,keys],i)=>{const rows=keys.filter(k=>Object.hasOwn(s,k)).map(k=>{seen.add(k);return [k,s[k]];});if(!rows.length)return null;
    return h('details',{'data-key':key,open:open.size?open.has(key):i<2},h('summary',{},h('span',{i18n:key}),h('span',{class:'badge'},rows.length)),
     h('dl',{},rows.map(([k,v])=>[h('dt',{},I18N.has('d.'+k)?t('d.'+k):k),h('dd',{class:'num'},diagValue(k,v))])));}).filter(Boolean));
   const rest=Object.keys(s).filter(k=>!seen.has(k)&&!(Array.isArray(s[k])&&typeof s[k][0]==='object'));
   if(rest.length)groups.append(h('details',{'data-key':'diag.g_other'},h('summary',{},h('span',{i18n:'diag.g_other'}),h('span',{class:'badge'},rest.length)),
    h('dl',{},rest.map(k=>[h('dt',{},I18N.has('d.'+k)?t('d.'+k):k),h('dd',{class:'num'},diagValue(k,s[k]))]))));
   raw.textContent=JSON.stringify(s,null,2);updated.textContent=t('diag.updated',{t:new Date().toLocaleTimeString(I18N.lang)});
  }};
}
function logsPage(){
 const out=h('pre',{},''),rows=[],state=h('p',{class:'help'});
 let cursor=0;
 async function read(){
  const d=await command('logs.get',{after:cursor,limit:8});
  if(d.overwritten)rows.push(t('logs.overwritten'));
  for(const r of d.records)rows.push(JSON.stringify(r.module==='system'&&r.event==='watchdog'?{...r,message:t('diag.watchdog_reboot')}:r));
  cursor=d.next;out.textContent=rows.join('\n')||t('logs.empty');return d;
 }
 const refresh=h('button',{type:'button',class:'btn'},icon('auto'),h('span',{i18n:'diag.log_refresh'}));
 refresh.onclick=()=>run(refresh,read,null);
 const all=h('button',{type:'button',class:'btn'},h('span',{i18n:'logs.load_all'}));
 all.onclick=()=>run(all,async()=>{for(let i=0;i<8;i++){const d=await read();if(cursor>=d.latest)break;}},null);
 const dl=h('button',{type:'button',class:'btn'},h('span',{i18n:'logs.download'}));
 dl.onclick=()=>run(dl,async()=>{
  for(let i=0;i<8;i++){const d=await read();if(cursor>=d.latest)break;}
  const url=URL.createObjectURL(new Blob([rows.join('\n')],{type:'text/plain;charset=utf-8'})),a=h('a',{href:url,download:'device-log.txt'});
  document.body.append(a);a.click();a.remove();URL.revokeObjectURL(url);
 },null);
 return {el:h('div',{class:'stack'},pageHead('nav.logs','logs.lead'),
  card('diag.logs',null,h('p',{class:'help',i18n:'logs.hint'}),h('div',{class:'form-actions'},refresh,all,dl),out,state))};
}
function maintenancePage(dev){
 const reboot=h('button',{type:'button',class:'btn'},h('span',{i18n:'sys.reboot'}));
 reboot.onclick=async()=>{if(await confirmDialog({title:t('sys.reboot_q',{m:dev.model}),body:t('sys.reboot_body'),ok:t('sys.reboot')}))run(reboot,()=>api('/reboot',{}),'toast.rebooting');};
 const items=[h('div',{class:'item'},h('div',{class:'grow'},h('b',{i18n:'sys.reboot'}),h('span',{i18n:'sys.reboot_desc'})),reboot)];
 let reset;
 if(dev.factoryReset||UI.caps?.config_recovery){
  reset=h('button',{type:'button',class:'btn danger'},h('span',{i18n:'sys.reset'}));
  reset.onclick=async()=>{
   if(resetPending||!await confirmDialog({title:t('sys.reset_q'),body:t(UI.caps?.config_recovery?'recovery.confirm':'sys.reset_body'),ok:t('sys.reset'),danger:true})||resetPending)return;
   resetPending=true;resetAccepted=false;resetGeneration++;resetUptime=UI.confirmed?.uptime_seconds;resetSawPending=false;
   await run(reset,async()=>{
    try{const result=await api('/device/reset',{full:true});resetAccepted=true;resetGeneration++;return result;}
    catch(e){resetPending=false;throw e;}
   },'toast.resetting');
   reset.disabled=resetPending;
  };
  items.push(h('div',{class:'item'},h('div',{class:'grow'},h('b',{i18n:'sys.reset'}),h('span',{i18n:'sys.reset_desc'})),reset));
 }
 return {el:h('div',{class:'stack'},pageHead('nav.maintenance','sys.lead'),backupCard(),
  h('section',{class:'card danger-zone'},h('div',{class:'list'},items),
   h('p',{class:'help',style:'margin-top:12px',i18n:dev.buttonHintKey},t(dev.buttonHintKey)))),
  status(){if(reset)reset.disabled=resetPending;}};
}
/* Slots shared by every "General" page. */
function nameCard(){
 const input=h('input',{id:'deviceName',maxLength:63}),meta=h('div');
 const form=h('form',{id:'nameForm',class:'stack'},fieldRow('gen.name',
  h('div',{class:'row'},h('div',{class:'grow'},input),h('button',{class:'btn primary',type:'submit'},h('span',{i18n:'common.save'}))),
  h('button',{class:'btn ghost',type:'button',onclick:e=>run(e.currentTarget,async()=>{await api('/deviceName',{name:''});await UI.reload();input.value=UI.state?.name||'';},'common.saved')},h('span',{i18n:'gen.default_name'}))),meta);
 onSubmit(form,()=>{if(new TextEncoder().encode(input.value).length>63)throw Error(t('gen.name_long'));return api('/deviceName',{name:input.value});});
 const language=h('select',{id:'deviceLanguage'},h('option',{value:'zh-CN'},'简体中文'),h('option',{value:'en'},'English'));
 const languageForm=h('form',{class:'stack',hidden:!UI.caps?.device_language},fieldRow('gen.language',language),info('gen.language_help'),h('button',{class:'btn',type:'submit'},h('span',{i18n:'common.save'})));
 onSubmit(languageForm,()=>command('config.set',{section:'device',values:{language:language.value}}));
 return {el:card('gen.identity',null,form,languageForm),init(s){input.value=s.name||'';language.value=s.language||'zh-CN';},status(s){meta.replaceChildren(summary([['gen.device_id',h('span',{class:'num'},prettyMac(s.device_id))],['gen.uptime',duration(s.uptime_seconds||0)]]));}};
}
function authCard(){
 const input=h('input',{id:'webPassword',type:'password',maxLength:32,autocomplete:'new-password',placeholder:t('auth.ph')});
 const state=h('span',{class:'badge'});
 const form=h('form',{id:'authForm',class:'stack'},fieldRow('auth.password',input,h('p',{class:'help',i18n:'auth.help'})),
  h('div',{class:'form-actions'},h('button',{class:'btn primary',type:'submit'},h('span',{i18n:'auth.set'}))));
 onSubmit(form,()=>{if(input.value&&!/^[\x20-\x7e]{8,32}$/.test(input.value))throw Error(t('auth.invalid'));return api('/auth/password',{password:input.value});});
 const c=card('auth.title','auth.lead',form);c.querySelector('.card-head').append(state);
 return {el:c,status(s){state.className='badge '+(s.web_password_set?'ok':'');state.textContent=t(s.web_password_set?'auth.on':'auth.off');}};
}
function clockCard({load,save,status}){
 const sum=h('div'),tz=h('select'),ntp=h('input',{maxLength:63,required:true,placeholder:'ntp1.aliyun.com'});
 const fill=()=>{const v=tz.value;tz.replaceChildren(...TZ.map(m=>h('option',{value:m},tzLabel(m))));tz.value=v;};fill();I18N.onChange(fill);
 const form=h('form',{class:'stack'},h('div',{class:'fields'},
   fieldRow('clock.tz',tz),fieldRow('clock.ntp',ntp)),
  info('clock.hint'),h('div',{class:'form-actions'},h('button',{class:'btn primary',type:'submit'},h('span',{i18n:'common.save'}))));
 onSubmit(form,()=>save({timezone_minutes:Number(tz.value),ntp_host:ntp.value.trim()}));
 return {el:card('clock.title',null,sum,h('div',{style:'margin-top:16px'},form)),
  async show(options={}){const c=await load(options);if(!c||options.current&&!options.current())return;tz.value=String(c.timezone_minutes||480);ntp.value=c.ntp_host||'';},
  status(s){sum.replaceChildren(summary(status(s)));}};
}

/* ---------- Boot ---------- */
let initialized=false,failures=0,lastSuccess=0,loading=null,loadingGeneration=0;
let stopped=false,pollingStarted=false,pollTimer=null,pollGeneration=0;
const lockedControls=new Map();
function lockHome(){if(!UI.embedded)return;const home=$('homePage');if(home)home.inert=true;for(const el of document.querySelectorAll('#homePage button,#homePage input,#homePage select')){if(!lockedControls.has(el))lockedControls.set(el,el.disabled);el.disabled=true;}}
function unlockHome(){if(UI.embedded){const home=$('homePage');if(home)home.inert=false;}for(const [el,disabled] of lockedControls)el.disabled=disabled;lockedControls.clear();}
async function load(priority){
 const lifecycle=pollGeneration,resetVersion=resetGeneration;
 try{
  const params=UI.statusParams(),generation=fullGeneration;
  const raw=await command('status.get',params,{priority});
  if(stopped||lifecycle!==pollGeneration)return null;
  failures=0;lastSuccess=Date.now();unlockHome();
  if(!params.view&&!raw.ota_mode){lastFullAt=Date.now();loadedGeneration=generation;}
  if(raw.ota_active&&raw.ota_mode){UI.pages.firmware?.status(raw);return raw;}
  if((!params.view&&(!UI.caps?.business_status||capabilityGeneration!==generation))||UI.caps?.config_recovery||resetPending){
   const caps=await command('capabilities.get',{}, {priority}),config=await command('config.get',{}, {priority});
   if(stopped||lifecycle!==pollGeneration||resetVersion!==resetGeneration)return null;
   const wasRecovery=!!UI.caps?.config_recovery;
   UI.acceptCapabilities(caps);
   capabilityGeneration=generation;
   if(UI.identityChanged)return null;
   UI.caps.config_recovery=!!UI.caps.config_recovery||config.migration_state==='confirmation_required';
   if(resetPending&&raw.reboot_pending)resetSawPending=true;
   if(wasRecovery&&!UI.caps.config_recovery&&!raw.reboot_pending)initialized=false;
   const restarted=resetSawPending||(Number.isFinite(resetUptime)&&raw.uptime_seconds<resetUptime);
   if(resetAccepted&&!raw.reboot_pending&&(!UI.caps.config_recovery||restarted)){
    resetPending=resetAccepted=false;
    if(!UI.caps.config_recovery)initialized=false;
    toast(t(UI.caps.config_recovery?'recovery.unconfirmed':'recovery.ready'),!!UI.caps.config_recovery);
   }
  }
  if(generation!==fullGeneration)return null;
  const s=UI.acceptStatus(raw,params.view==='live');
  if(mayM1)readMcuVersion().catch(()=>{});
  $('title').textContent=UI.device.model+' '+(s.name||'');
  renderChips(s);renderBanners(s,false);
  if(!initialized){for(const p of Object.values(UI.pages))if(p.init)p.init(s);initialized=true;}
  let diagnosticState=s;
  if((UI.activePage()==='diagnostics'||UI.device.diagnosticPages?.includes(UI.activePage()))&&UI.caps?.actions?.includes('diagnostics.get')){
   const routeGenerationAtRead=routeGeneration,signal=routeRead?.signal;
   try{
    const reply=await command('diagnostics.get',{},signal?{signal}:{});
    if(stopped||lifecycle!==pollGeneration||routeGenerationAtRead!==routeGeneration)return null;
    diagnosticState=mergeDiagnostic(s,diagnosticSnapshot(reply));
   }catch(error){if(error?.name==='AbortError')return null;}
  }
  for(const [key,p] of Object.entries(UI.pages))if(p.status&&(key==='diagnostics'?key===UI.activePage():key===UI.activePage()||key.startsWith('slot_')&&UI.activePage()==='device'))p.status(key==='diagnostics'?diagnosticState:s);
  if(UI.device.onStatus)UI.device.onStatus(s);
  if(diagnosticState!==s&&UI.device.diagnosticStatus)UI.device.diagnosticStatus(s,diagnosticState);
  return raw;
 }catch(e){if(!stopped&&lifecycle===pollGeneration&&++failures>=3&&(!lastSuccess||Date.now()-lastSuccess>=30000)){lockHome();renderBanners(null,true);}return null;}
}
function pollLoad(priority=2){if(stopped)return Promise.resolve(null);if(!loading){loadingGeneration=fullGeneration;loading=load(priority).finally(()=>{loading=null;});}return loading;}
window.addEventListener('pagehide',()=>{stopped=true;pollGeneration++;clearTimeout(pollTimer);pollTimer=null;});
async function refreshStatus(generation=pollGeneration,resuming=false){
 if(stopped||generation!==pollGeneration)return;
 if(!document.hidden||!initialized){
  // An old request may settle after cache restoration. Retire it before the
  // required full read; its former loop cannot schedule another timer.
  if(resuming&&loading)await loading;
  if(stopped||generation!==pollGeneration)return;
  await pollLoad();
 }
 if(!stopped&&generation===pollGeneration)pollTimer=setTimeout(function refresh(){
  if(generation===pollGeneration)pollTimer=null;
  return refreshStatus(generation);
 },UI.device.interval||5000);
}
window.addEventListener('pageshow',()=>{
 if(!stopped)return;
 stopped=false;UI.invalidateStatus();
 if(pollingStarted)refreshStatus(pollGeneration,true);
});
let fullReload=null;
UI.reload=({full=false,priority=2}={})=>{
 if(full)UI.invalidateStatus();
 if(!loading||loadingGeneration===fullGeneration)return loading||pollLoad(priority);
 if(!fullReload)fullReload=(async()=>{await loading;return pollLoad(priority);})().finally(()=>{fullReload=null;});
 return fullReload;
};
document.addEventListener('visibilitychange',()=>{if(UI.device&&!document.hidden&&!stopped)UI.reload({full:true});});
const capabilityIdentity=value=>value?.device_id&&value?.model&&value?.firmware_version&&(value.variant||value.firmware_variant)?[String(value.device_id).replace(/[:-]/g,'').toLowerCase(),value.model,value.firmware_version,value.variant||value.firmware_variant].join('/') : '';
UI.invalidateIdentity=identity=>{
 const previous=capabilityIdentity(UI.caps),next=capabilityIdentity(identity);
 if(previous&&next&&previous!==next&&!UI.identityChanged){
  UI.identityChanged=true;UI.identityGeneration=(UI.identityGeneration||0)+1;
  routeGeneration++;routeRead?.abort();pollGeneration++;
  for(const id of ['homePage','settingsPage']){const box=$(id);if(box){box.inert=true;box.setAttribute('inert','');}}
  const en=I18N.lang==='en';
  $('banners').replaceChildren(h('div',{class:'banner warn',role:'alert'},h('p',{},en?'Device firmware changed. Unsaved contents are preserved. Refresh to load the new controls.':'设备固件已变更。未保存内容已保留，请刷新以加载新界面。'),h('button',{class:'btn',type:'button',onclick:()=>location.reload()},en?'Refresh page':'刷新页面')));
 }
};
UI.acceptCapabilities=caps=>{UI.invalidateIdentity(caps);UI.caps={...caps};UI.pages.firmware?.capabilities?.(UI.caps);};
UI.showRoute=showRoute;
UI.start=async dev=>{
 UI.device=dev;lockHome();
 const first=dev.groups.flatMap(g=>g[1])[0];
 document.body.prepend(header(dev),
  h('nav',{class:'tabs','aria-label':'Main'},
   h('a',{href:'#home','data-tab':'home'},icon('home'),h('span',{i18n:'nav.home'})),
   h('a',{href:'#settings/'+first,'data-tab':'settings'},icon('sliders'),h('span',{i18n:'nav.settings'}))),
  h('div',{id:'banners'}));
 UI.caps=await command('capabilities.get').catch(()=>({actions:[]}));capabilityGeneration=fullGeneration;
 if(dev.capabilities)dev.capabilities(UI.caps);
 if(!UI.embedded&&UI.caps.model==='daikin'&&UI.caps.firmware_variant==='hub'){
  const tabs=document.querySelector('.tabs');tabs.classList.add('hub-tabs');
  tabs.append(h('a',{class:'hub-management-entry',href:'/iotcloud/#/'},icon('link'),h('span',{i18n:'nav.hub'})));
 }
 const pages=$('pages');
 // Pages the framework builds for every product.
 const builders={network:wifiPage,mqtt:mqttPage,firmware:firmwarePage,diagnostics:diagnosticsPage,maintenance:maintenancePage,logs:logsPage};
 for(const [key,build] of Object.entries(builders)){const p=build(dev);UI.pages[key]=p;pages.append(h('div',{'data-settings-page':key,hidden:true},p.el));}
 // Automation pages, when the device advertises the capability.
 Object.assign(UI.pages,window.automationPages(dev));
 // Product pages: the product supplies show()/status(); the markup is either
 // already in the document or appended below.
 for(const [key,p] of Object.entries(dev.pages))UI.pages[key]=Object.assign(UI.pages[key]||{},p);
 for(const [key,p] of Object.entries(UI.pages)){
  let box=document.querySelector('[data-settings-page="'+key+'"]');
  if(!box){box=h('div',{'data-settings-page':key,hidden:true});pages.append(box);}
  if(p.el)box.append(p.el);
 }
 for(const slot of document.querySelectorAll('[data-slot]')){const p=slot.dataset.slot==='name'?nameCard():authCard();UI.pages['slot_'+slot.dataset.slot]=p;slot.replaceWith(p.el);}
 buildNav(dev);
 window.addEventListener('hashchange',showRoute);showRoute();
 I18N.apply();
 I18N.onChange(()=>{if(UI.state){renderChips(UI.state);renderBanners(UI.state,false);for(const p of Object.values(UI.pages))if(p.status)p.status(UI.state);}});
 pollingStarted=true;refreshStatus();
};
})();
