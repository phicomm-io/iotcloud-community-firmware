/* Public provisioning contract. Passwords and session tokens are never encoded into a URL. */
(()=>{
'use strict';
const VERSION=1,TTL=120000;
const bytes=s=>new TextEncoder().encode(s).length;
function localHost(host){
 const h=host.toLowerCase().replace(/^\[|\]$/g,'');
 if(h==='localhost'||h==='::1')return true;
 if(h.includes(':'))return /^(fc|fd)[0-9a-f]{2}:|^fe[89ab][0-9a-f]:/.test(h);
 if(!/^\d+\.\d+\.\d+\.\d+$/.test(h))return false;
 const p=h.split('.').map(Number);if(p.some(n=>n>255))return false;
 return p[0]===10||p[0]===127||(p[0]===172&&p[1]>=16&&p[1]<=31)||(p[0]===192&&p[1]===168)||(p[0]===169&&p[1]===254);
}
function deviceURL(raw){
 if(typeof raw!=='string'||!raw.trim()||raw.length>256)throw Error('device');
 const value=raw.trim(),url=new URL(value.includes('://')?value:'http://'+value);
 if(!['http:','https:'].includes(url.protocol)||!localHost(url.hostname)||url.username||url.password||url.search||url.hash||url.pathname!=='/')throw Error('device');
 return url;
}
function platformOrigin(raw){
 const u=new URL(raw);
 if(raw!==u.origin||!(u.protocol==='https:'||(u.protocol==='http:'&&['localhost','127.0.0.1','[::1]'].includes(u.hostname))))throw Error('origin');
 return u.origin;
}
function nonce(){const a=new Uint8Array(32);crypto.getRandomValues(a);return Array.from(a,n=>n.toString(16).padStart(2,'0')).join('');}
function readTicket(hash,now=Date.now()){
 if(!hash.startsWith('#iotcloud-import='))return null;
 if(hash.length>2048)throw Error('ticket');
 const p=new URLSearchParams(hash.slice(1));
 if([...p.keys()].sort().join(',')!=='expires,host,iotcloud-import,origin,port,user')throw Error('ticket');
 const id=p.get('iotcloud-import'),expires=Number(p.get('expires')),origin=platformOrigin(p.get('origin'));
 if(!/^[a-f0-9]{64}$/.test(id)||!Number.isSafeInteger(expires)||expires<=now||expires>now+TTL)throw Error('ticket');
 const visible=config({v:VERSION,host:p.get('host'),port:Number(p.get('port')),user:p.get('user'),password:'',protocol:'tmcp',expires},now);
 return {id,expires,origin,host:visible.host,port:visible.port,user:visible.user};
}
function config(value,now=Date.now()){
 if(!value||typeof value!=='object'||Array.isArray(value)||Object.keys(value).sort().join(',')!=='expires,host,password,port,protocol,user,v')throw Error('config');
 const {v,host,port,user,password,protocol,expires}=value;
 if(v!==VERSION||protocol!=='tmcp'||typeof host!=='string'||!host||bytes(host)>127||!/^([a-zA-Z0-9.-]+|\[[a-fA-F0-9:]+\])$/.test(host)||host.startsWith('.')||host.endsWith('.')||typeof user!=='string'||bytes(user)>63||/[\x00-\x1f\x7f]/.test(user)||typeof password!=='string'||bytes(password)>256||/[\x00-\x1f\x7f]/.test(password)||!Number.isInteger(port)||port<1||port>65535||!Number.isSafeInteger(expires)||expires<=now||expires>now+TTL)throw Error('config');
 return {v,host,port,user,password,protocol,expires};
}
function packet(e,{source,origin,id,expires},type){
 const d=e.data;return Date.now()<expires&&e.source===source&&e.origin===origin&&!!d&&typeof d==='object'&&d.type===type&&d.id===id;
}
globalThis.IotProvision=Object.freeze({VERSION,TTL,deviceURL,platformOrigin,nonce,readTicket,config,packet});
})();
