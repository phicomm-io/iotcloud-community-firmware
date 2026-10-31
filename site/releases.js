'use strict';
const english = document.documentElement.lang === 'en';
const language = english ? 'en' : 'zh-CN';
const t = (zh, en) => english ? en : zh;
const siteBase = new URL(document.body.dataset.siteBase || './', location.href);
const status = document.getElementById('release-status'), source = document.getElementById('source');
const el = (tag, text) => { const n = document.createElement(tag); if (text) n.textContent = text; return n; };
const categories = {feature: t('新功能', 'Features'), improvement: t('功能改进', 'Improvements'), bugfix: t('问题修复', 'Fixes'), security: t('安全更新', 'Security'), breaking: t('重要变化', 'Breaking changes')};
const variantLabels = {full: t('完整版', 'Full'), 'no-frontend-resources': t('精简版，保留配网页', 'Compact, with setup page'), 'fresh-air': t('新风版', 'Fresh air'), 'fresh-air-no-frontend-resources': t('新风精简版，保留配网页', 'Compact fresh air, with setup page'), hub: t('中枢版', 'Hub')};
let generation = 0, active = null;
function link(label, path, base) { const a = el('a', label); a.href = base + path; a.rel = 'noopener'; return a; }
function reviewedNotes(release, manifest) {
  const note = el('div'); note.className = 'notes'; note.dataset.loaded = '1';
  const record = manifest.releases.find(r => r.tag === release.tag);
  if (!record) { note.append(el('p', t('此版本说明尚未整理。', 'Notes for this release are not available yet.'))); return note; }
  if (record.history_status === 'insufficient') note.append(el('p', t('只有提交链接，资料不足，暂不能生成升级差异。', 'There is not enough reviewed information to compare this release.')));
  if (record.history_status === 'pending') note.append(el('p', t('部分说明仍在整理，以下仅展示已审核内容。', 'Some notes are pending. Only reviewed changes are shown.')));
  const rows = manifest.changes.filter(c => c.version === release.version && c.component !== 'service');
  if (!rows.length && record.history_status === 'no_user_changes') note.append(el('p', t('本版没有新增用户可见变化，与上一版使用相同实现。', 'No new user-visible changes in this release.')));
  for (const kind of ReleaseNotes.types) {
    const items = rows.filter(c => c.type === kind); if (!items.length) continue;
    note.append(el('h4', categories[kind]));
    const list = el('ul');
    for (const item of items) {
      const row = el('li'); row.append(el('b', item.title[language] || t('翻译待审核', 'Translation pending review')), el('p', item.description[language] || t('说明待审核', 'Description pending review')));
      row.append(el('small', item.scope.models.map(x => x.toUpperCase()).join(english ? ', ' : '、') + ' · ' + item.scope.variants.map(v => variantLabels[v] || v).join(' / ')));
      list.append(row);
    }
    note.append(list);
  }
  return note;
}
const modelNames = {m1: 'M1', a1: 'A1', tc1: 'TC1', daikin: 'DAIKIN', s30: 'S30-V1B'};
const variantDescriptions = {
 full: t('完整本地网页，适合常规使用。', 'Complete local web interface for normal use.'),
 'no-frontend-resources': t('仅保留配网页，节省设备存储；日常管理使用平台。', 'Keeps the Wi-Fi setup page and saves flash; use the platform for daily management.'),
 'fresh-air': t('A1 外接新风风机适配，包含完整网页；普通 A1 无需选择。', 'A1 external fresh-air fan support with the full web interface.'),
 'fresh-air-no-frontend-resources': t('A1 新风功能，仅保留配网页。', 'A1 fresh-air support with only the setup page.'),
 hub: t('大金中枢功能，需要按中枢说明配置。', 'DAIKIN hub functions; follow the hub setup guide.')
};
const fileTypes = {
 signed: [t('签名 OTA', 'Signed OTA'), t('已安装社区固件的日常升级包，由设备校验签名。', 'Normal update for community firmware; the device checks its signature.')],
 raw: [t('原始 OTA', 'Raw OTA'), t('原厂迁移或教程明确要求的应用包；不是全片镜像。', 'Application image for stock migration or instructions that explicitly require it; not a full flash image.')],
 full: [t('完整镜像', 'Full image'), t('有线首次安装或恢复使用，不能在网页当作 OTA 上传。', 'For wired installation or recovery; never upload it as a web OTA package.')],
 rbl: ['RBL', t('S30-V1B 原厂免拆安装或社区 URL OTA；设备主动通过 HTTP 下载。', 'S30-V1B stock network installation or community URL OTA; downloaded by the device over HTTP.')],
 stock_ua: ['STOCK_UA', t('S30-V1B 串口安装，从 0x11000 写入，保留本机原厂启动器；不能用于网页 OTA。', 'S30-V1B serial installation at 0x11000, preserving its stock bootloader; not for web OTA.')]
};
let dialogSequence = 0;
function deviceDownload(model, release, variants, snapshot, native = false) {
 const name=modelNames[model]||model.toUpperCase(),card=el('article');card.className='firmware-device';card.dataset.downloadModel=model;
 card.append(el('h4',name),el('p',release.tag));
 const button=el('button',t('下载', 'Download'));button.type='button';button.className='btn';button.setAttribute('aria-haspopup','dialog');
 const dialog=el('dialog');dialog.className='firmware-dialog';dialog.id='firmware-download-'+(++dialogSequence);button.setAttribute('aria-controls',dialog.id);
 const header=el('div');header.className='firmware-dialog-head';
 const heading=el('h2',name+' · '+release.tag);heading.id=dialog.id+'-title';dialog.setAttribute('aria-labelledby',heading.id);
 const close=el('button',t('关闭', 'Close'));close.type='button';close.className='btn ghost';
 close.addEventListener('click',()=>typeof dialog.close==='function'?dialog.close():dialog.removeAttribute('open'));
 header.append(heading,close);dialog.append(header,el('p',t('先核对设备、固件变体与刷机方式，再选择文件类型。', 'Check the device, firmware variant and installation method before choosing a file.')));
 for(const [variant,value] of variants){
  const section=el('section');section.className='firmware-variant';section.dataset.variant=variant;
  section.append(el('h3',variantLabels[variant]||variant),el('p',variantDescriptions[variant]||t('本设备原生固件。', 'Native firmware for this device.')));
  for(const [kind,info] of Object.entries(fileTypes)){
   const file=value.downloads[kind];if(!file)continue;
   const row=el('div');row.className='firmware-file';
   row.append(link(t('下载 ', 'Download ')+info[0],file.path,native?'':snapshot.source),el('p',info[1]),el('code',file.path.split('/').pop()));
   if(file.size)row.append(el('small',file.size.toLocaleString(language)+' B'));
   if(file.sha256)row.append(el('small','SHA-256: '+file.sha256));
   section.append(row);
  }
  dialog.append(section);
 }
 const slug=model==='s30'?'s30-flash':model==='daikin'?'daikin-guide':model+'-flash';
 const guide=el('a',t('刷机说明', 'Installation guide'));guide.href=new URL('guides/'+(english?'en/':'')+slug+'.html',siteBase);dialog.append(guide);
 if(native&&variants[0][1].checksums)dialog.append(link(t('SHA256 校验文件', 'SHA256 checksums'),variants[0][1].checksums,''));
 button.addEventListener('click',()=>{if(typeof dialog.showModal==='function')dialog.showModal();else dialog.setAttribute('open','');});
 dialog.addEventListener('close',()=>button.focus());card.append(button,dialog);return card;
}
function releaseCard(release, latest, snapshot, candidate = false) {
 const box=el(latest||candidate?'article':'details');box.className='release';
 const head=el(latest||candidate?'h3':'summary',release.tag);box.append(head);
 if(latest||candidate){const badge=el('span',candidate?t('Beta（RC）','Beta (RC)'):t('稳定最新版','Latest stable'));badge.className='tag';head.append(badge);}
 const devices=el('div');devices.className='firmware-devices';
 for(const [model,original] of Object.entries(release.models))devices.append(deviceDownload(model,release,[['full',original],...Object.entries(original.variants||{})],snapshot));
 const nativeNode=document.getElementById('native-downloads');
 if(nativeNode){
  const native=JSON.parse(nativeNode.textContent).releases.find(r=>r.tag===release.tag);
  if(native)for(const [model,value] of Object.entries(native.models))if(!release.models[model])devices.append(deviceDownload(model,release,[['full',value]],snapshot,true));
 }
 const notes=el('details');notes.className='release-notes';notes.append(el('summary',t('本版变化','Changes in this release')),reviewedNotes(release,snapshot.manifest));
 box.append(devices,link(t('SHA256 校验文件','SHA256 checksums'),`/releases/${release.tag}/SHA256SUMS`,snapshot.source),link(t('原始单版本说明 ↗','Original release notes ↗'),release.notes,snapshot.source),notes);
 return box;
}
async function loadCatalog() {
  const epoch = ++generation; active?.abort(); active = new AbortController();
  document.getElementById('latest').replaceChildren(); document.getElementById('archive').replaceChildren();
  status.textContent = t('正在校验发布说明与版本归档…', 'Verifying release notes and archive…');
  try {
    const snapshot = await ReleaseNotesClient.snapshot(source.value, {signal: active.signal});
    if (epoch !== generation) return;
    const releases = snapshot.catalog.releases;
    const stable = releases.find(r => r.tag === snapshot.catalog.stable);
    if (stable) document.getElementById('latest').append(releaseCard(stable, true, snapshot));
    else throw Error(t('正式最新版不在当前归档中。', 'The latest release is missing from this archive.'));
    for (const release of releases) document.getElementById('archive').append(releaseCard(release, false, snapshot));
    status.textContent = t(`共 ${releases.length} 个版本 · 展示已审核用户说明 · 历史文件按版本保留`, `${releases.length} releases · reviewed notes · versioned archives`);
  } catch (error) {
    if (epoch !== generation || error.code === 'REQUEST_ABORTED') return;
    status.textContent = error.code === 'SOURCE_MISMATCH' ? t('说明与固件目录不同步，请切换完整来源后重试。', 'Notes and firmware do not match. Choose another complete source.') : t('暂时无法校验发布数据，请切换来源或稍后重试。', 'Release data could not be verified. Change source or try again later.');
  }
}
// Candidate downloads stay on this hash-verified archive. Mirror selection
// applies only to stable/history and must never promote a candidate to latest.
async function loadCandidateCatalog() {
  const target = document.getElementById('candidate-releases');
  const message = document.getElementById('candidate-release-status');
  if (!target || !message) return;
  try {
    const snapshot = await ReleaseNotesClient.snapshot('', {channel: 'candidate'});
    const candidates = snapshot.catalog.releases
      .filter(release => release.channel === 'candidate' && release.prerelease === true)
      .sort((a, b) => ReleaseNotes.compareVersions(b.version, a.version)).slice(0, 1);
    target.replaceChildren();
    for (const release of candidates) target.append(releaseCard(release, false, snapshot, true));
    message.textContent = candidates.length
      ? t('RC 用于 Beta 测试。', 'RC builds are for beta testing.')
      : t('目前没有可用候选版。', 'No release candidate is currently available.');
  } catch (error) {
    target.replaceChildren();
    message.textContent = error.code === 'CANDIDATE_UNAVAILABLE'
      ? t('目前没有可用候选版。', 'No release candidate is currently available.')
      : t('暂时无法校验候选版目录，请稍后重试或查看候选版说明。', 'Candidate downloads could not be verified. Try later or read the candidate release notes.');
  }
}
source.addEventListener('change', loadCatalog);
loadCatalog();
loadCandidateCatalog();

// Controller releases include development previews: /releases/latest excludes them.
// Keep this independent of the Wi-Fi catalog and mirror selector.
async function loadM1ControllerReleases(){
 const target=document.getElementById('m1-mcu-versions');
 const message=document.getElementById('m1-mcu-status');
 if(!target||!message)return;
 const repo='https://github.com/phicomm-io/phicomm_m1_mcu_atsamd20';
 const attachments=[['m1-mcu-ota.bin',t('应用 OTA', 'Application OTA')],['m1-mcu-ota.json',t('OTA 清单', 'OTA manifest')],['m1-mcu-debug-recovery.zip',t('调试 / 恢复包', 'Debug / recovery archive')],['SHA256SUMS',t('SHA256 校验', 'SHA256 checksums')],['release.json',t('构建记录', 'Build record')]];
 async function read(url){
  const controller=new AbortController();
  const timer=setTimeout(()=>controller.abort(),8000);
  try{
   const response=await fetch(url,{signal:controller.signal});
   if(!response.ok)throw Error('HTTP '+response.status);
   const data=await response.json();
   if(!Array.isArray(data))throw Error('Invalid releases');
   return data.filter(r=>{
    if(!r||r.draft||!Array.isArray(r.assets))return false;
    try{
     const version=ReleaseNotes.parseVersion(r.tag_name);
     return !version.suffix&&r.tag_name==='v'+version.version;
    }catch{return false;}
   });
  }finally{clearTimeout(timer);}
 }
 let versions,snapshot=false;
 try{versions=await read('https://api.github.com/repos/phicomm-io/phicomm_m1_mcu_atsamd20/releases?per_page=100');}
 catch{
  snapshot=true;
  try{versions=await read(new URL('guides/assets/firmware/m1-mcu/releases.json', siteBase));}
  catch{message.textContent=t('主控版本暂时无法读取，请使用上方 GitHub Releases 入口。', 'Controller releases are unavailable. Use the GitHub Releases link above.');return;}
 }
 // Use the shared version order, including bare RC and numbered candidates.
 versions.sort((a,b)=>ReleaseNotes.compareVersions(b.tag_name,a.tag_name));
 target.replaceChildren();
 for(const [index,release] of versions.entries()){
  const box=el('details');box.className='release';box.open=index===0;
  const preview=release.prerelease||ReleaseNotes.parseVersion(release.tag_name).prerelease;
  box.append(el('summary',release.tag_name+(preview?t(' · 开发预览', ' · Development preview'):t(' · 已发布', ' · Published'))));
  const links=el('div');links.className='actions';
  const add=(label,url)=>{const a=el('a',label);a.href=url;a.rel='noopener noreferrer';links.append(a);};
  for(const [name,label] of attachments){
   if(release.assets.some(a=>a.name===name))add(label,repo+'/releases/download/'+release.tag_name+'/'+name);
  }
  add(t('GitHub 发布页', 'GitHub release'),repo+'/releases/tag/'+release.tag_name);
  box.append(links);target.append(box);
 }
 message.textContent=(snapshot?t('GitHub API 暂不可用，显示本站版本快照；最新版本请查看 GitHub。 ', 'GitHub API is unavailable. Showing the site snapshot; check GitHub for the latest release. '):t('已从 GitHub 同步。 ', 'Synced from GitHub. '))+t(`共 ${versions.length} 个主控版本（含开发预览）。`, `${versions.length} controller releases, including development previews.`);
}
loadM1ControllerReleases();
