/* APP1 / IOTW5651. Shared by the device editor and the offline Pages converter. */
(()=>{
'use strict';
const roles=['canvas','background','title','value','unit','body','accent','line','chartA','chartB','chartC','danger'];
const widgetRoles=roles.slice(1,6),themes=['forest','ink','ocean','amber','plum','paper'],wallpapers=['none','mountains','aurora','dunes','custom'];
const palettes=[
 ['101611','1b251d','d9e5d0','f0f3e8','b1bbab','e1e9db','b7dca5','354335','b7dca5','8ebfcd','dcb981','efab91'],
 ['101216','20242b','d8dce4','f4f5f7','a7afbd','e0e4ec','c5d3e8','3b424f','c5d3e8','96c9c0','d8b79a','f1a6a6'],
 ['0c1721','162d3d','c5e1ed','edf8ff','a1bdcf','d5e8f2','8fd9ef','2e4e61','8fd9ef','a8cf9d','ecc393','ffb4a0'],
 ['211811','35271d','e9d4bd','fff2df','c5ae98','ecdfce','efc28c','554232','efc28c','a6c4b2','b8b5df','f4a59c'],
 ['1b1523','2d233a','e0d1ed','f8efff','b9a8c8','e8dcee','d5b5f0','4b3b5b','d5b5f0','99cbd2','e4c297','f5a4b5'],
 ['eeeee6','fcfcf6','394536','1c2a1c','5c6857','34412f','426c3a','cbd1c1','426c3a','346f86','9c682c','a13828']
].map(p=>Object.fromEntries(roles.map((k,i)=>[k,'#'+p[i]])));
const color=c=>typeof c==='string'&&/^#[a-f\d]{6}$/i.test(c),uint=(v,max)=>Number.isInteger(v)&&v>=0&&v<=max;
function defaults(){return {revision:1,theme:'forest',colors:{},wallpaper:'none',cardOpacity:80,wallpaperDim:24,custom_crc:0,language:'zh-CN',pageRailHeight:0,items:{}};}
const railHeight=a=>a.pageRailHeight||12;
function validate(a){
 if(!a||!['zh-CN','en'].includes(a.language??'zh-CN')||![0,6,12,20].includes(a.pageRailHeight??0)||!uint(a.revision,0xffffffff)||!a.revision||!themes.includes(a.theme)||!wallpapers.includes(a.wallpaper)||!uint(a.cardOpacity,100)||!uint(a.wallpaperDim,70)||!uint(a.custom_crc,0xffffffff)||!a.colors||typeof a.colors!=='object'||Array.isArray(a.colors)||!a.items||typeof a.items!=='object'||Array.isArray(a.items)||Object.keys(a.items).length>32)throw Error('Invalid appearance');
 for(const [k,v]of Object.entries(a.colors))if(!roles.includes(k)||!color(v))throw Error('Invalid color');
 for(const [id,it]of Object.entries(a.items)){
  if(!/^[1-9]\d*$/.test(id)||!uint(Number(id),65535)||!it||typeof it!=='object'||Array.isArray(it))throw Error('Invalid component');
  for(const [k,v]of Object.entries(it))if(k==='opacity'?!uint(v,100):!widgetRoles.includes(k)||!color(v))throw Error('Invalid component color');
 }return a;
}
function encode(a){
 validate(a);const p=new Uint8Array(704),v=new DataView(p.buffer);
 p.set([65,80,80,49]);v.setUint32(4,a.revision,true);p[10]=themes.indexOf(a.theme);p[11]=wallpapers.indexOf(a.wallpaper);p[12]=a.cardOpacity;p[13]=a.wallpaperDim;
 const rgb=(offset,s)=>p.set([parseInt(s.slice(1,3),16),parseInt(s.slice(3,5),16),parseInt(s.slice(5),16)],offset);
 let mask=0;roles.forEach((k,i)=>{if(a.colors[k]){mask|=1<<i;rgb(16+i*3,a.colors[k]);}});v.setUint16(8,mask,true);v.setUint32(52,a.custom_crc,true);
 const items=Object.entries(a.items);p[14]=items.length;p[56]=a.language==='en'?1:0;p[57]=a.pageRailHeight??0;
 items.forEach(([id,it],i)=>{const offset=64+20*i;v.setUint16(offset,Number(id),true);p[offset+3]=it.opacity??255;widgetRoles.forEach((k,j)=>{if(it[k]){p[offset+2]|=1<<j;rgb(offset+4+j*3,it[k]);}});});return p;
}
function decode(p){
 if(p.length!==704||String.fromCharCode(...p.slice(0,4))!=='APP1'||p[9]>15||p[14]>32||p[15]||p[56]>1||p.slice(58,64).some(Boolean))throw Error('Invalid APP1');
 const v=new DataView(p.buffer,p.byteOffset,p.length),a=defaults(),rgb=o=>'#'+Array.from(p.slice(o,o+3),x=>x.toString(16).padStart(2,'0')).join('');
 Object.assign(a,{revision:v.getUint32(4,true),theme:themes[p[10]],wallpaper:wallpapers[p[11]],cardOpacity:p[12],wallpaperDim:p[13],custom_crc:v.getUint32(52,true),language:p[56]?'en':'zh-CN',pageRailHeight:p[57]});
 roles.forEach((k,i)=>{if(v.getUint16(8,true)&1<<i)a.colors[k]=rgb(16+i*3);});
 for(let i=0;i<p[14];i++){
  const o=64+20*i,id=v.getUint16(o,true),it={};if(Object.hasOwn(a.items,id)||p[o+2]>31||p[o+19])throw Error('Invalid APP1 component');
  if(p[o+3]!==255)it.opacity=p[o+3];widgetRoles.forEach((k,j)=>{if(p[o+2]&1<<j)it[k]=rgb(o+4+j*3);});a.items[id]=it;
 }return validate(a);
}
const resolve=a=>({...palettes[themes.indexOf(a.theme)],...a.colors});
function surface(a,it,wallpaper){const custom=a.items[it.id]||{};return custom.opacity??(wallpaper?a.cardOpacity:custom.background||(it.type!==0&&it.h<4)?100:0);}
function widgetColors(a,it){const p=resolve(a),c=a.items[it.id]||{};return Object.fromEntries(widgetRoles.map(k=>[k,c[k]||p[k]]));}
function crc(p){let c=0xffffffff;for(const b of p){c^=b;for(let i=0;i<8;i++)c=(c>>>1)^((c&1)?0xedb88320:0);}return (c^0xffffffff)>>>0;}
function parseWallpaper(p){
 const v=new DataView(p.buffer,p.byteOffset,p.length);
 if(p.length!==204992||String.fromCharCode(...p.slice(0,8))!=='IOTW5651'||v.getUint16(12,true)!==240||v.getUint16(14,true)!==427||v.getUint32(20,true)!==204960||v.getUint32(24,true)||v.getUint32(28,true)!==crc(p.slice(0,28))||v.getUint32(16,true)!==crc(p.slice(32)))throw Error('Invalid .bwp image');
 return {crc:v.getUint32(16,true),width:240,height:427};
}
function packWallpaper(rgba){
 if(rgba.length!==240*427*4)throw Error('Invalid image dimensions');const p=new Uint8Array(204992),v=new DataView(p.buffer);
 p.set([73,79,84,87,53,54,53,49]);v.setUint16(12,240,true);v.setUint16(14,427,true);v.setUint32(20,204960,true);
 for(let i=0;i<240*427;i++)v.setUint16(32+i*2,((rgba[i*4]&248)<<8)|((rgba[i*4+1]&252)<<3)|(rgba[i*4+2]>>3),true);
 v.setUint32(16,crc(p.subarray(32)),true);v.setUint32(28,crc(p.subarray(0,28)),true);return p;
}
function imageURL(p){
 parseWallpaper(p);const canvas=document.createElement('canvas');canvas.width=240;canvas.height=427;
 const c=canvas.getContext('2d'),image=c.createImageData(240,427),v=new DataView(p.buffer,p.byteOffset,p.length);
 for(let i=0;i<240*427;i++){const rgb=v.getUint16(32+i*2,true);image.data.set([Math.round((rgb>>11)*255/31),Math.round(((rgb>>5)&63)*255/63),Math.round((rgb&31)*255/31),255],i*4);}
 c.putImageData(image,0,0);return canvas.toDataURL('image/png');
}
async function convertFile(file){
 if(!file||!['image/png','image/jpeg','image/webp'].includes(file.type)||file.size>5*1024*1024)throw Error('Use PNG, JPEG or WebP up to 5 MB');
 const image=await createImageBitmap(file);
 try{
  if(!image.width||!image.height||image.width*image.height>20000000)throw Error('Image exceeds 20 megapixels');
  const canvas=document.createElement('canvas');canvas.width=240;canvas.height=427;const c=canvas.getContext('2d');
  c.fillStyle='#101611';c.fillRect(0,0,240,427);const scale=Math.max(240/image.width,427/image.height),w=240/scale,h=427/scale;
  c.drawImage(image,(image.width-w)/2,(image.height-h)/2,w,h,0,0,240,427);
  return packWallpaper(c.getImageData(0,0,240,427).data);
 }finally{image.close();}
}
const mix=(a,b,t,n)=>{let c=0;for(let k=0;k<24;k+=8)c|=Math.floor((((a>>k)&255)*(n-t)+((b>>k)&255)*t)/n)<<k;return c;};
function ridge(x,p){for(let i=2;i<p.length;i+=2)if(x<=p[i])return p[i-1]+Math.trunc((p[i+1]-p[i-1])*(x-p[i-2])/(p[i]-p[i-2]));return p.at(-1);}
function presetPixel(kind,x,y){
 if(kind===1){let c=mix(0x6d9391,0x102e36,y,854);if((x-355)**2+(y-153)**2<42**2)c=mix(c,0xe4dfbc,6,10);if(y>ridge(x,[0,374,95,265,151,313,261,212,380,351,480,293]))c=0x557777;if(y>ridge(x,[0,517,110,385,213,480,327,339,480,504]))c=0x30545b;if(y>ridge(x,[0,589,139,510,266,639,356,565,480,596]))c=0x193b43;if(y>690+Math.trunc((x-240)**2/750))c=0x102b34;return c;}
 if(kind===2){const c=mix(0x10233b,0x171c34,x+y,1334),center=125+Math.trunc(y/4)+Math.trunc((y-400)**2/3000),d=Math.abs(x-center),t=d<125?Math.trunc((125-d)*65/125):0;return mix(c,y<430?0x75c6bb:0x8e85bd,t,100);}
 let c=mix(0x55485d,0xd7ae86,y,854);if((x-349)**2+(y-217)**2<49**2)c=mix(c,0xeed5ad,75,100);if(y>400+Math.trunc((x-240)**2/600))c=0xb48a74;if(y>480+Math.trunc(x/3))c=mix(0xc69d77,0x66504e,y-480,374);if(y>650+Math.trunc((x-240)**2/750))c=0x775754;if(y>749+Math.trunc(x/5))c=0x4c3b42;return c;
}
const presetCache={};
function presetURL(name){
 if(presetCache[name])return presetCache[name];const kind=wallpapers.indexOf(name);if(kind<1||kind>3)return null;
 const canvas=document.createElement('canvas');canvas.width=480;canvas.height=854;const c=canvas.getContext('2d');if(!c?.createImageData)return null;
 const image=c.createImageData(480,854);for(let y=0;y<854;y++)for(let x=0;x<480;x++){const v=presetPixel(kind,x,y);image.data.set([v>>16,(v>>8)&255,v&255,255],(y*480+x)*4);}c.putImageData(image,0,0);return presetCache[name]=canvas.toDataURL('image/png');
}
globalThis.ScreenAppearance={roles,widgetRoles,themes,wallpapers,palettes,defaults,railHeight,validate,encode,decode,resolve,surface,widgetColors,crc,parseWallpaper,packWallpaper,imageURL,convertFile,presetPixel,presetURL};
})();
