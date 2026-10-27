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
function releaseCard(release, latest, snapshot, candidate = false) {
  const box = el(latest || candidate ? 'article' : 'details'); box.className = 'release';
  const head = el(latest || candidate ? 'h3' : 'summary', release.tag); box.append(head);
  if (latest || candidate) { const badge = el('span', candidate ? t('Beta（RC）· 不建议使用', 'Beta (RC) · Not recommended') : t('稳定最新版', 'Latest stable')); badge.className = 'tag'; head.append(badge); }
  const table = el('table'); table.className = 'downloads';
  const tr = el('tr'); for (const text of [t('型号', 'Model'), t('固件下载', 'Firmware')]) tr.append(el('th', text)); table.append(tr);
  for (const [model, original] of Object.entries(release.models)) {
    for (const [variant, value] of [['full', original], ...Object.entries(original.variants || {})]) {
      const row = el('tr'); row.append(el('td', model.toUpperCase() + ' · ' + (variantLabels[variant] || variant)));
      const cell = el('td');
      for (const [kind, label] of [['signed', t('签名 OTA', 'Signed OTA')], ['raw', t('原始 OTA', 'Raw OTA')], ['full', t('完整镜像', 'Full image')]]) if (value.downloads[kind]) cell.append(link(label, value.downloads[kind].path, snapshot.source));
      row.append(cell); table.append(row);
    }
  }
  const notes = el('details'); notes.className = 'release-notes';
  notes.append(el('summary', t('本版变化', 'Changes in this release')), reviewedNotes(release, snapshot.manifest));
  box.append(table, link(t('SHA256 校验文件', 'SHA256 checksums'), `/releases/${release.tag}/SHA256SUMS`, snapshot.source),
    link(t('原始单版本说明 ↗', 'Original release notes ↗'), release.notes, snapshot.source), notes);
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
      ? t('RC 实际属于 Beta 测试版本，后续仍会有较大改动，不建议使用。', 'RC releases are beta builds. Major changes are still expected; using them is not recommended.')
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
