(()=>{'use strict';
const $=id=>document.getElementById(id),texts={
 title:['屏幕壁纸转换','Screen wallpaper converter'],intro:['选择图片，预览后下载 .bwp，再到设备的「屏幕 → 外观」上传。图片仅在此浏览器处理。','Choose an image, download .bwp, then upload it under Screen → Appearance on your device. Images stay in this browser.'],file:['图片文件','Image file'],format:['PNG、JPEG、WebP，最大 5 MB、2000 万像素。居中裁剪为 240 × 427 RGB565，屏幕显示为 480 × 854。','PNG, JPEG or WebP, up to 5 MB and 20 megapixels. Center-cropped to 240 × 427 RGB565, displayed at 480 × 854.'],dim:['壁纸压暗预览','Preview wallpaper dimming'],settings:['压暗与组件透明度由设备的外观设置控制，不写入图片。','Dimming and card opacity are device appearance settings; they are not baked into the image.'],download:['下载 .bwp','Download .bwp'],ready:['已转换 · 204,992 字节','Converted · 204,992 bytes'],failed:['图片无效或超出格式限制','Invalid image or format limit exceeded'],working:['正在转换…','Converting…']};
let language=navigator.language.startsWith('zh')?0:1,state='',url=null,job=0;
function render(){document.documentElement.lang=language?'en':'zh-CN';$('language').value=language?'en':'zh';document.querySelectorAll('[data-text]').forEach(el=>el.textContent=texts[el.dataset.text][language]);$('state').textContent=state?texts[state][language]:'';}
$('language').onchange=()=>{language=$('language').value==='en'?1:0;render();};
$('dim').oninput=()=>{$('overlay').style.opacity=$('dim').value/100;$('dimValue').textContent=$('dim').value+'%';};
$('file').onchange=async()=>{
 const current=++job,f=$('file').files[0];$('download').hidden=true;$('image').removeAttribute('src');if(url){URL.revokeObjectURL(url);url=null;}if(!f)return;state='working';render();
 try{if(f.size>5*1024*1024)throw Error();const p=/\.bwp$/i.test(f.name)?new Uint8Array(await f.arrayBuffer()):await ScreenAppearance.convertFile(f);ScreenAppearance.parseWallpaper(p);if(current!==job)return;
 $('image').src=ScreenAppearance.imageURL(p);url=URL.createObjectURL(new Blob([p],{type:'application/octet-stream'}));$('download').href=url;$('download').download=f.name.replace(/\.[^.]*$/,'').replace(/[^a-zA-Z0-9_-]/g,'_').slice(0,60)+'.bwp';$('download').hidden=false;state='ready';
 }catch{if(current!==job)return;state='failed';}render();
};render();})();
