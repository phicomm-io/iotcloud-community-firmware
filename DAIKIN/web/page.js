/* DAIKIN product adapter; shared styles/settings/service come from frontend.lock.json. */
(()=>{
'use strict';
const {$,h,t:unused}=UI;
const {t}=I18N;
$('verdict').remove();
$('screenSeg').closest('.grid').remove();
$('clockCard').remove();
document.querySelector('[data-settings-page="binding"]').remove();
const metrics=[['pm25','µg/m³'],['co2','ppm'],['temperature','°C'],['humidity','%'],['tvoc','ppb'],['formaldehyde','mg/m³']];
const cards={};
for(const [key,unit] of metrics){
 const value=h('span',{class:'num'},'—'),badge=h('span',{class:'badge'});
 const card=h('section',{class:'card metric','data-sensor':key},h('div',{class:'metric-head'},h('span',{class:'label'},key==='tvoc'?'TVOC':t('m.'+key)),badge),h('div',{class:'stat'},value,h('small',{},unit)));
 if(key==='formaldehyde')card.hidden=true;
 cards[key]={card,value,badge};$('metrics').append(card);
}
const led=h('input',{type:'checkbox'});
led.onchange=()=>UI.run(led,()=>command('indicator.set',{enabled:led.checked}));
$('homePage').append(h('section',{class:'card'},h('label',{class:'row'},led,'空气质量指示灯')));
const fw=h('div');
UI.start({model:'DAIKIN',kindKey:'kind.m1',icon:'air',factoryReset:true,mqttHostMax:127,interval:2000,
 otaName:'daikin-bry88ab.bin',haHintKey:'mqtt.ha_m1',otaHintKey:'fw.hint_m1',buttonHintKey:'sys.hint_m1',
 groups:[['grp.network',['network','mqtt']],['grp.device',['device']],['grp.system',['firmware','diagnostics','maintenance']]],
 nav:{network:'nav.wifi',mqtt:'nav.mqtt',device:'nav.general',firmware:'nav.firmware',diagnostics:'nav.diagnostics',maintenance:'nav.maintenance'},
 pages:{device:{},firmware:{show(){
  // This port uses ESP32 application images, not the MiCO signed package format.
  const page=document.querySelector('[data-settings-page="firmware"]');page.replaceChildren(fw);
  if(fw.childNodes.length)return;
  const input=h('input',{type:'file',accept:'.bin'}),result=h('p',{role:'status'});
  const upload=h('button',{class:'btn primary',type:'button'},'上传固件');
  upload.onclick=()=>UI.run(upload,async()=>{
   const file=input.files[0];if(!file)throw Error('请选择对应型号的 ESP32 application .bin');
   if(!window.confirm('确认安装未签名的 DAIKIN 固件？请勿上传 factory 完整镜像。'))return;
   result.textContent='正在上传…';
   const response=await fetch('/api/v1/ota/upload-unsigned',{method:'POST',headers:{'Content-Type':'application/octet-stream'},body:file});
   const data=await response.json();if(response.status!==202)throw Error(data.message);
   result.textContent='校验通过，设备即将重启';
  });
  fw.append(h('section',{class:'card stack'},h('h2',{},'DAIKIN BRY88AX151K 固件升级'),h('p',{class:'help'},'仅支持本型号 ESP32 application .bin；上传前请确认来源。'),input,upload,result));
 }}},
 diagGroups:[['diag.g_sensor',['sensors','formaldehyde_present','pm_sensor','hardware_model']]],
 onStatus(s){
  for(const [key] of metrics){const el=cards[key],sample=s.sensors?.[key];
   el.card.hidden=key==='formaldehyde'&&!s.formaldehyde_present;
   el.value.textContent=sample?.valid?Number(sample.value).toFixed(key==='formaldehyde'?3:key==='temperature'?1:0):'—';
   el.badge.textContent=sample?.valid?'':key==='co2'&&s.co2_warmup_seconds_remaining>0?'预热中（剩余 '+s.co2_warmup_seconds_remaining+' 秒）':'暂无有效数据';
  }
  led.checked=!!s.indicator_enabled;
 }
});
})();
