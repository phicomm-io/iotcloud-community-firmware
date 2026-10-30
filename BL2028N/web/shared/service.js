var DeviceAuth=(()=>{var N=Object.defineProperty;var Y=Object.getOwnPropertyDescriptor;var Z=Object.getOwnPropertyNames;var ee=Object.prototype.hasOwnProperty;var te=(t,c)=>{for(var n in c)N(t,n,{get:c[n],enumerable:!0})},re=(t,c,n,l)=>{if(c&&typeof c=="object"||typeof c=="function")for(let a of Z(c))!ee.call(t,a)&&a!==n&&N(t,a,{get:()=>c[a],enumerable:!(l=Y(c,a))||l.enumerable});return t};var ie=t=>re(N({},"__esModule",{value:!0}),t);var ae={};te(ae,{createDeviceAuth:()=>se,createDeviceWire:()=>H,decodeResponse:()=>B,verifyCapabilities:()=>$});var W=new TextEncoder,M=new TextDecoder("utf-8",{fatal:!0}),J=new Set(["__proto__"]),C=t=>t==="msgpack"?"application/msgpack":"application/json";function P(t){return t instanceof Uint8Array?t:Object.prototype.toString.call(t)==="[object ArrayBuffer]"?new Uint8Array(t):ArrayBuffer.isView(t)?new Uint8Array(t.buffer,t.byteOffset,t.byteLength):null}function ne(t){if(typeof t=="string")return"json";let c=P(t),n=c?.[0];return n>=128&&n<=143||n===222||n===223?"msgpack":"json"}function w(){throw Error("Invalid device wire payload")}function V(t){(t.includes("\0")||/[\uD800-\uDBFF](?![\uDC00-\uDFFF])|(?<![\uD800-\uDBFF])[\uDC00-\uDFFF]/u.test(t))&&w()}function E(t,c=0,n={n:0},l=!0){if((c>=12||++n.n>8192)&&w(),!(t===null||typeof t=="boolean")){if(P(t)){l||w();return}if(typeof t=="number"){(!Number.isFinite(t)||Number.isInteger(t)&&!Number.isSafeInteger(t))&&w();return}if(typeof t=="string"){V(t);return}(typeof t!="object"||ArrayBuffer.isView(t)||t instanceof ArrayBuffer||!Array.isArray(t)&&Object.getPrototypeOf(t)!==Object.prototype)&&w();for(let[a,i]of Object.entries(t))V(a),(J.has(a)||W.encode(a).length>=128)&&w(),E(i,c+1,n,l)}}function I(t,c="msgpack"){if(E(t,0,{n:0},c!=="json"),c==="json")return JSON.stringify(t);c!=="msgpack"&&w();let n=null,l=0;function a(e){n&&n.set(e,l),l+=e.length}function i(e){n&&(n[l]=e),l++}function p(e,o){for(let s=o-1;s>=0;s--)i(Math.floor(e/2**(8*s))&255)}function m(e,o){if(e===0&&o<128){i(o);return}if(e===1&&o<32){i(255-o);return}if(e===3&&o<32){i(160+o);return}if((e===4||e===5)&&o<16){i((e===4?144:128)+o);return}let s;if(e<=1){if(s=e===0?o<=255?1:o<=65535?2:o<=4294967295?4:8:o<=127?1:o<=32767?2:o<=2147483647?4:8,i((e===0?204:208)+(s===1?0:s===2?1:s===4?2:3)),e===0)p(o,s);else for(let f=s-1;f>=0;f--)i(255-(Math.floor(o/2**(8*f))&255));return}s=e===3&&o<=255?1:o<=65535?2:4,i(e===3?s===1?217:s===2?218:219:e===4?s===2?220:221:s===2?222:223),p(o,s)}function _(e){let o=W.encode(e);m(3,o.length),a(o)}function b(e){if(e===null){i(192);return}if(typeof e=="boolean"){i(194+Number(e));return}if(typeof e=="number"){if(Number.isSafeInteger(e)&&!Object.is(e,-0)){m(e<0?1:0,e<0?-1-e:e);return}i(203);let f=new Uint8Array(8);new DataView(f.buffer).setFloat64(0,e),a(f);return}if(typeof e=="string"){_(e);return}let o=P(e);if(o){let f=o.length<=255?1:o.length<=65535?2:4;i(f===1?196:f===2?197:198),p(o.length,f),a(o);return}if(Array.isArray(e)){m(4,e.length);for(let f of e)b(f);return}let s=Object.entries(e);m(5,s.length);for(let[f,y]of s)_(f),b(y)}return b(t),l>1048576&&w(),n=new Uint8Array(l),l=0,b(t),n}function oe(t,c=ne(t),{onNumber:n,allowArray:l=!1}={}){let a;if(c==="json")a=JSON.parse(typeof t=="string"?t:M.decode(P(t)));else{let b=function(y){y>i.length-m&&w();let u=m;return m+=y,u},e=function(y,u=!1){let r=0,g=b(y);for(let h=g;h<g+y;h++)r=r*256+(u?255-i[h]:i[h]);return Number.isSafeInteger(r)||w(),u?-1-r:r},o=function(y){let u=b(y),r;if(y===8)r=p.getFloat64(u);else if(y===4)r=p.getFloat32(u);else{let g=p.getUint16(u),h=g>>10&31,k=g&1023;h===31&&w(),r=(g&32768?-1:1)*(h?k+1024:k)*2**(h?h-25:-24)}return Number.isFinite(r)||w(),r},s=function(y,u,r){return n?.(y,u,r),y},f=function(y=0,u=[]){(y>=12||++_>8192)&&w();let r=i[b(1)],g,h;if(r<128)return s(r,!0,u);if(r>=224)return s(r-256,!0,u);if(r===192)return null;if(r===194||r===195)return r===195;if((r&224)===160)g=3,h=r&31;else if((r&240)===128)g=5,h=r&15;else if((r&240)===144)g=4,h=r&15;else if(r>=217&&r<=219)g=3,h=e(1<<r-217);else if(r>=196&&r<=198)g=2,h=e(1<<r-196);else if(r===220||r===221)g=4,h=e(r===220?2:4);else if(r===222||r===223)g=5,h=e(r===222?2:4);else{if(r>=204&&r<=207)return s(e(1<<r-204),!0,u);if(r>=208&&r<=211){let v=1<<r-208;return s(e(v,!!(i[m]&128)),!0,u)}else{if(r===202||r===203)return s(o(r===202?4:8),!1,u);w()}}if(g===2){let v=b(h);return i.slice(v,v+h)}if(g===3){let v=b(h),T=M.decode(i.subarray(v,v+h));return V(T),T}if(h>8192&&w(),g===4){let v=[];for(let T=0;T<h;T++)v.push(f(y+1,n?[...u,T]:u));return v}let k={};for(let v=0;v<h;v++){let T=f(y+1);(typeof T!="string"||Object.hasOwn(k,T)||J.has(T)||W.encode(T).length>=128)&&w(),k[T]=f(y+1,n?[...u,T]:u)}return k};c!=="msgpack"&&w();let i=P(t);(!i||!i.length||i.length>1048576)&&w();let p=new DataView(i.buffer,i.byteOffset,i.byteLength),m=0,_=0;a=f(),m!==i.length&&w()}return E(a),(!a||typeof a!="object"||!l&&Array.isArray(a)||P(a))&&w(),a}async function B(t,{allowArray:c=!1}={}){let n=t.headers?.get("Content-Type")?.split(";")[0];return t.status===503&&!["application/msgpack","application/json"].includes(n)?(await t.arrayBuffer(),{code:"busy"}):n==="application/msgpack"?oe(await t.arrayBuffer(),"msgpack",{allowArray:c}):t.json()}var z=(t,c=0)=>Object.assign(Error(t),{code:t,status:c}),U=t=>String(t||"").replace(/[:-]/g,"").toLowerCase();function R({fetcher:t=globalThis.fetch,origin:c="",format:n="json"}={}){return{discover:async()=>null,get format(){return n},get identity(){return null},headers:()=>({Accept:C(n),"Content-Type":C(n)}),encode:l=>I(l,n),decodeResponse:B,request(l,{value:a,beforeSend:i,...p}={}){return i?.(),t(c+l,{...p,...a===void 0?{}:{body:I(a,n)},headers:{Accept:C(n),...a===void 0?{}:{"Content-Type":C(n)},...p.headers}})}}}function H({fetcher:t=globalThis.fetch,origin:c="",expectedIdentity:n={}}={}){let l=null,a=null,i=null,p=!1,m=()=>{};function _(e,o){if(!e||typeof e!="object"||Array.isArray(e)||e.api_version!==1)throw z("Invalid device discovery");let s=U(e.device_id),f=e.model;if(!/^[a-f0-9]{12}$/.test(s)||!["a1","m1","tc1","daikin","s30-v1b"].includes(f))throw z("Invalid device identity");if(n.device_id&&U(n.device_id)!==s||n.mac&&U(n.mac)!==s||n.model&&n.model!==f)throw z("Device identity changed");let y=["wire_version","wire_formats","default_wire_format"].some(r=>Object.hasOwn(e,r)),u="json";if(y){if(e.wire_version!==1||e.default_wire_format!=="msgpack"||!Array.isArray(e.wire_formats)||!e.wire_formats.includes("msgpack")||typeof e.firmware_version!="string"||!e.firmware_version||typeof(e.variant||e.firmware_variant)!="string"||!(e.variant||e.firmware_variant))throw z("Unsupported device wire contract");u="msgpack"}else if(o==="msgpack")throw z("Missing binary discovery contract");return{format:u,identity:{device_id:s,model:f,firmware_version:e.firmware_version||e.version||"",variant:e.variant||e.firmware_variant||""},data:e}}async function b({signal:e,force:o=!1,legacyPassword:s=""}={}){if(i)return i;if(l&&!o)return a;i=(async()=>{let f=await t(c+"/api/v1/info",{signal:e,credentials:"omit",redirect:"error",cache:"no-store",headers:{Accept:"application/msgpack, application/json"}}),y=!1;if(f.status===401&&s){y=!0;let h="Basic "+btoa(String.fromCharCode(...new TextEncoder().encode("admin:"+s)));f=await t(c+"/api/v1/info",{signal:e,credentials:"omit",redirect:"error",cache:"no-store",headers:{Accept:"application/json",Authorization:h}})}if(!f.ok)throw z(f.status===503?"busy":f.status===401?"unauthorized":"HTTP "+f.status,f.status);let u=await B(f);if(u?.code!=="ok")throw z(u?.code||"Invalid device discovery",f.status);let r=f.headers?.get("Content-Type")?.split(";")[0],g=_(u.data,r==="application/msgpack"?"msgpack":"json");if(y&&(g.format!=="json"||r==="application/msgpack"))throw z("Invalid legacy discovery");return a&&JSON.stringify(a)!==JSON.stringify(g.identity)&&m(),l=g.format,p=y&&l==="json",a=Object.freeze(g.identity),a})();try{return await i}finally{i=null}}return{discover:b,get legacyBasicVerified(){return p},get format(){return l},get identity(){return a},setOnIdentityChange(e){m=e},headers:()=>({Accept:C(l),"Content-Type":C(l)}),encode(e){if(!l)throw z("Discovery required");return I(e,l)},decodeResponse:B,async request(e,{value:o,beforeSend:s,...f}={}){return await b({signal:f.signal}),s?.(),t(c+e,{credentials:"omit",...f,redirect:"error",cache:"no-store",...o===void 0?{}:{body:I(o,l)},headers:{Accept:C(l),...o===void 0?{}:{"Content-Type":C(l)},...f.headers}})}}}function $(t,c){if(!c||!t||U(t.device_id)!==c.device_id||t.model!==c.model||t.firmware_version!==c.firmware_version||(t.variant||t.firmware_variant)!==c.variant)throw z("Device capabilities identity changed");return t}var O=(t,c)=>Object.assign(Error(t==="unauthorized"?"Sign in to this device":t==="busy"?"Device busy; try again":t),{code:t,status:c}),G=t=>"Basic "+btoa(String.fromCharCode(...new TextEncoder().encode("admin:"+t)));function se({fetcher:t=globalThis.fetch,origin:c="",onRequired:n=()=>{},allowLegacy:l=!0,wire:a=R({fetcher:t,origin:c})}={}){let i="",p="",m=!1,_=!1,b=null,e=0,o=null,s=!1,f=!1,y=-1,u=()=>i?{Authorization:"Bearer "+i}:p?{Authorization:p}:{},r=({requireLogin:d=!0}={})=>{e++,o?.abort(),o=null,i=p="",m=!1,s=!1,_=d,b=null};function g(){_||(_=!0,n())}function h(d,x){if(!(arguments.length>1&&x!==u().Authorization)&&d===401){if(s&&i)return r({requireLogin:!1}),k("").catch(()=>{});i=p="",m=!1,g()}}async function k(d=""){if(b)return b;let x=i;i=p="",m=!1,s=!1;let A=e,D=new AbortController,q=setTimeout(()=>D.abort(),5e3);o=D,b=(async()=>{if(x&&await L(x),A!==e||(await a.discover({legacyPassword:d,signal:D.signal,force:f}),f=!0,y===e&&A===e-1&&(A=e),A!==e))throw O("cancelled",0);let j=await a.request("/api/v1/auth/login",{method:"POST",signal:D.signal,credentials:"omit",redirect:"error",cache:"no-store",...a.format==="json"&&a.legacyBasicVerified&&d?{headers:{Authorization:G(d)}}:{},value:{password:d}});if(A!==e&&!j.ok)throw O("cancelled",0);if(j.status===404&&l&&a.format==="json"){p=d?G(d):"",m=!0,s=d==="",_=!1;return}if(j.status===401)throw i=p="",m=!1,s=!1,g(),O("unauthorized",401);if(!j.ok)throw O(j.status===503?"busy":"HTTP "+j.status,j.status);let S=await B(j),F=S?.data?.token;if(A!==e)throw typeof F=="string"&&/^[a-f0-9]{32}$/.test(F)&&L(F),O("cancelled",0);if(S.code==="ok"&&S.data?.password_required===!1&&F===""){m=!0,s=!0,_=!1;return}if(S.code!=="ok"||typeof F!="string"||!/^[a-f0-9]{32}$/.test(F))throw O("Invalid device session",j.status);i=F,p="",m=!1,s=d==="",_=!1})();try{return await b}finally{clearTimeout(q),A===e&&(b=null,o=null)}}async function v(){let d=e;if(i||m)return d;if(_)throw O("unauthorized",401);if(await k(""),d!==e&&y!==e)throw O("cancelled",0);return e}async function T(d,x={}){let A=await v();if(A!==e||x.signal?.aborted)throw O("cancelled",0);let D=u().Authorization,q=await a.request(d,{...x,credentials:"omit",redirect:"error",cache:"no-store",headers:{...x.headers,...u()},beforeSend(){if(A!==e||x.signal?.aborted)throw O("cancelled",0);x.beforeSend?.()}});return A===e&&await h(q.status,D),q}async function K(){let d=s,x=e,A=u().Authorization;try{i&&await a.request("/api/v1/auth/logout",{method:"POST",credentials:"omit",redirect:"error",cache:"no-store",headers:{Authorization:A},value:{}})}finally{x===e&&A===u().Authorization&&(r({requireLogin:!d}),d||n())}}function L(d){let x,A=new Promise(D=>{x=setTimeout(D,2e3)});try{return Promise.race([A,Promise.resolve(a.request("/api/v1/auth/logout",{method:"POST",keepalive:!0,credentials:"omit",redirect:"error",cache:"no-store",headers:{Authorization:"Bearer "+d},value:{}})).catch(()=>{})]).finally(()=>clearTimeout(x))}catch{return clearTimeout(x),Promise.resolve()}}function Q(){let d=i;return r({requireLogin:!1}),d?L(d):Promise.resolve()}async function X(d,{relogin:x=!1,beforeSend:A}={}){let D=await v();if(D!==e)throw O("cancelled",0);let q=u().Authorization,j=await T("/api/v1/command",{method:"POST",beforeSend:A,value:{action:"config.set",request_id:globalThis.crypto?.randomUUID?crypto.randomUUID():Date.now().toString(36)+"-"+Math.random().toString(36).slice(2),params:{section:"auth",values:{password:d}}}});if(!j.ok)throw O(j.status===401?"unauthorized":j.status===503?"busy":"HTTP "+j.status,j.status);let S=await B(j);if(!["ok","accepted"].includes(S?.code))throw O(S?.code||"Invalid device response",j.status);return D!==e||q!==u().Authorization||(r({requireLogin:d!==""&&!x}),x?await k(d):d!==""&&n()),S}return a.setOnIdentityChange?.(()=>{o?(e++,y=e,i=p="",m=!1,s=!1,_=!1):r({requireLogin:!1})}),{wire:a,login:k,ensure:v,fetch:T,headers:u,observe:h,logout:K,release:Q,changePassword:X,reset:r,setOnRequired(d){n=d},get authenticated(){return!!i||m},get needsLogin(){return _}}}return ie(ae);})();

// Standalone device pages use an in-memory local session. Embedded pages use
// their parent's transport and must never send HTTP credentials over MQTT.
(()=>{
 if(typeof window==='undefined'||window.deviceParentTransport===true)return;
 let prompt=null;
 const tr=(en,zh)=>globalThis.I18N?.lang==='en'?en:zh;
 function signIn(){
  if(prompt)return prompt;
  prompt=new Promise(resolve=>{
   const box=document.createElement('dialog'),form=document.createElement('form'),title=document.createElement('h2'),input=document.createElement('input'),hint=document.createElement('p'),submit=document.createElement('button');
   title.textContent=tr('Sign in to device','登录设备');input.type='password';input.autocomplete='current-password';input.placeholder=tr('Device password','设备密码');input.setAttribute('aria-label',title.textContent);submit.type='submit';submit.textContent=tr('Sign in','登录');
   form.className='stack';submit.className='btn primary';hint.setAttribute('role','alert');
   form.append(title,input,hint,submit);box.append(form);document.body.append(box);
   box.addEventListener('cancel',event=>event.preventDefault());
   form.onsubmit=async event=>{event.preventDefault();submit.disabled=true;hint.textContent='';const password=input.value;input.value='';try{await auth.login(password);box.close?.();box.remove();prompt=null;resolve();globalThis.UI?.reload?.({full:true});}catch(error){hint.textContent=error.status===503?tr('Device busy. Try again.','设备繁忙，请稍后重试。'):tr('Could not sign in. Check the password.','登录失败，请检查密码。');}finally{submit.disabled=false;}};
   if(box.showModal)box.showModal();else box.setAttribute('open','');input.focus();
  });return prompt;
 }
 const fetcher=window.fetch.bind(window),wire=DeviceAuth.createDeviceWire({fetcher});
 const auth=DeviceAuth.createDeviceAuth({fetcher,wire,onRequired:()=>{signIn();}});
 async function ready(){try{await auth.ensure();}catch(error){if(error.status!==401)throw error;await signIn();}}
 window.DeviceSession={...auth,ready,login:auth.login,async fetch(path,options){await ready();return auth.fetch(path,options);},async logout(){await auth.logout();},headers:auth.headers};
 window.addEventListener('pagehide',()=>auth.release());
})();

const checkDeviceWrite=()=>{if(globalThis.DeviceSession?.wire?.identity)globalThis.UI?.invalidateIdentity?.(DeviceSession.wire.identity);if(globalThis.UI?.identityChanged)throw Object.assign(Error(I18N.lang==='en'?'Device firmware changed. Refresh this page before saving.':'设备固件已变更，请刷新页面后再保存。'),{code:'identity_changed'});};
const deviceFetch=(path,options={})=>{const write=options.method&&!['GET','HEAD'].includes(options.method.toUpperCase()),check=()=>{if(write)checkDeviceWrite();};check();return globalThis.DeviceSession?DeviceSession.fetch(path,{...options,beforeSend:check}):fetch(path,options);};
// Share only overlapping reads, never completed values or mutations. Each
// subscriber owns cancellation; an unrelated subscriber keeps the wire alive.
const deviceReads=new Map();let deviceReadEpoch=0;
const readAbort=()=>new DOMException('Read superseded','AbortError');
// One lane through response decoding. Writes and confirmations take precedence
// over queued background reads; cancellation only retires read subscribers.
globalThis.DeviceHttpQueue=true;
const deviceJobs=[];let deviceWorking=false;
function deviceSchedule(work,{signal,priority=2}={}){
 if(signal?.aborted)return Promise.reject(readAbort());
 return new Promise((resolve,reject)=>{
  const job={priority,async run(){signal?.removeEventListener('abort',abort);try{resolve(await work());}catch(error){reject(error);}}};
  const abort=()=>{const index=deviceJobs.indexOf(job);if(index<0)return;deviceJobs.splice(index,1);signal.removeEventListener('abort',abort);reject(readAbort());};
  signal?.addEventListener('abort',abort,{once:true});deviceJobs.push(job);
  if(deviceWorking)return;
  deviceWorking=true;
  (async()=>{while(deviceJobs.length){deviceJobs.sort((a,b)=>a.priority-b.priority);await deviceJobs.shift().run();}deviceWorking=false;})();
 });
}
function command(action,params={},options={}){
 if(!action.endsWith('.get'))return deviceCommand(action,params);
 const signal=options.signal;
 if(signal?.aborted)return Promise.reject(readAbort());
 const stable=JSON.stringify(params,(_,value)=>value&&typeof value==='object'&&!Array.isArray(value)?Object.fromEntries(Object.keys(value).sort().map(key=>[key,value[key]])):value);
 const key=deviceReadEpoch+':'+action+':'+stable;
 let read=deviceReads.get(key);
 if(!read){
  const controller=new AbortController();read={controller,users:0};
  read.promise=deviceCommand(action,params,{signal:controller.signal,priority:options.priority}).finally(()=>{if(deviceReads.get(key)===read)deviceReads.delete(key);});
  deviceReads.set(key,read);
 }
 read.users++;
 return new Promise((resolve,reject)=>{
  let settled=false;
  const finish=(error,value)=>{if(settled)return;settled=true;signal?.removeEventListener('abort',abort);if(!--read.users&&error?.name==='AbortError'){if(deviceReads.get(key)===read)deviceReads.delete(key);read.controller.abort();}if(error)reject(error);else resolve(globalThis.structuredClone?globalThis.structuredClone(value):JSON.parse(JSON.stringify(value)));};
  const abort=()=>finish(readAbort());signal?.addEventListener('abort',abort,{once:true});
  read.promise.then(value=>finish(null,value),error=>finish(error));
  if(signal?.aborted)abort();
 });
}
function deviceCommand(action,params={},options={}){
 const write=!action.endsWith('.get');
 if(write)deviceReadEpoch++;
 return deviceSchedule(()=>deviceSend(action,params,options),{signal:write?undefined:options.signal,priority:write?0:options.priority});
}
async function deviceSend(action,params={},options={}){
 const identity=globalThis.UI?.identityGeneration||0;
 const request={action,params};
 if(!action.endsWith('.get'))request.request_id=globalThis.crypto?.randomUUID?crypto.randomUUID():Date.now().toString(36)+'-'+Math.random().toString(36).slice(2);
 const controller=new AbortController(),timer=setTimeout(()=>controller.abort(),20000);
 const abort=()=>controller.abort();options.signal?.addEventListener('abort',abort,{once:true});
 try{
 if(options.signal?.aborted)throw readAbort();
 const response=await deviceFetch('/api/v1/command',{method:'POST',signal:controller.signal,...(globalThis.DeviceSession?{value:request}:{headers:{'Content-Type':'application/json'},body:JSON.stringify(request)}),cache:'no-store'});
 if(identity!==(globalThis.UI?.identityGeneration||0))throw Object.assign(Error('Device identity changed'),{code:'identity_changed'});
 let reply;try{reply=await (globalThis.DeviceAuth?DeviceAuth.decodeResponse(response):response.json());}catch(error){if(response.status!==503)throw error;reply={code:'busy'};}
 if(identity!==(globalThis.UI?.identityGeneration||0))throw Object.assign(Error('Device identity changed'),{code:'identity_changed'});
 if(!response.ok||!['ok','accepted'].includes(reply.code)){const key='error.'+reply.code;const error=Error(globalThis.I18N?.has(key)?I18N.t(key):reply.message||reply.code||('HTTP '+response.status));error.code=reply.code;throw error;}
 if(action==='capabilities.get'&&globalThis.DeviceSession?.wire.format==='msgpack')DeviceAuth.verifyCapabilities(reply.data,DeviceSession.wire.identity);
 if(!action.endsWith('.get')){deviceReadEpoch++;globalThis.UI?.invalidateStatus?.();}
 return reply.data||{};
 }finally{clearTimeout(timer);options.signal?.removeEventListener('abort',abort);}
}
async function api(path,data,options={}){
 if(path==='/auth/password'&&globalThis.DeviceSession){checkDeviceWrite();deviceReadEpoch++;return deviceSchedule(async()=>{const reply=await DeviceSession.changePassword(data.password,{beforeSend:checkDeviceWrite});deviceReadEpoch++;globalThis.UI?.invalidateStatus?.();return reply.data||{};},{priority:0});}
 if(path==='/status')return command('status.get',{},options);
 if(path==='/sensors/average')return command('sensors.average.get',{},options);
 if(path==='/power')return command('power.get',{},options);
 if(path==='/wifi/scan')return command(data===undefined?'wifi.scan.get':'wifi.scan');
 const domains={'/binding/config':'binding','/binding/status':'binding.status','/binding/probe':'binding.probe','/linkage/config':'linkage','/light/advanced':'light_effect','/sleep/config':'night','/time/config':'time'};
 if(domains[path])return command(domains[path]+(path==='/binding/probe'?'':data===undefined?'.get':'.set'),data||{},options);
 const direct={'/reboot':'system.reboot','/device/reset':'system.reset','/ota':'ota.start','/childLock':'child_lock.set','/child_lock':'child_lock.set','/led':'indicator.set','/outlet':'outlet.set','/fan':'fan.set','/fan/config':'fan.set','/light':'light.set','/light/config':'light.set'};
 if(direct[path]){if(['/light','/light/config'].includes(path)&&data.brightness!==undefined)data={...data,brightness:Math.round(data.brightness*100/255)};return command(direct[path],data);}
 if(path==='/display/brightness')return command('display.set',{brightness:data.level*25});
 if(path==='/socket'){for(let index=0;index<data.states.length;index++)await command('outlet.set',{index,on:!!data.states[index]});return {};}
 const sections={'/wifi/config':'wifi','/mqtt/config':'mqtt','/deviceName':'device','/auth/password':'auth','/socketNames':'socket','/mqtt/report/freq':'report','/power/coefficient':'meter','/ntp/config':'ntp'};
 if(sections[path]){
  let values={...data};
  if(path==='/power/coefficient')values={coefficient_x100:Number(data)};
  if(path==='/mqtt/config'){delete values.topic_mode;delete values.clear_password;if(data.clear_password)values.password='';else if(!values.password)delete values.password;}
  if(path==='/wifi/config'){delete values.open_network;if(data.open_network)values.password='';else if(!values.password)delete values.password;}
  return command('config.set',{section:sections[path],values});
 }
 throw Error(globalThis.I18N?.t('common.unsupported')||'Unsupported operation');
}
