'use strict';
const status = document.getElementById('release-status'), source = document.getElementById('source');
const el = (tag, text) => { const n = document.createElement(tag); if (text) n.textContent = text; return n; };
const categories = {feature: '新功能', improvement: '功能改进', bugfix: '问题修复', security: '安全更新', breaking: '重要变化'};
const variantLabels = {full: '完整版', 'no-frontend-resources': '精简版，保留配网页', 'fresh-air': '新风版', 'fresh-air-no-frontend-resources': '新风精简版，保留配网页'};
let generation = 0, active = null;
function link(label, path, base) { const a = el('a', label); a.href = base + path; a.rel = 'noopener'; return a; }
function reviewedNotes(release, manifest) {
  const note = el('div'); note.className = 'notes'; note.dataset.loaded = '1';
  const record = manifest.releases.find(r => r.tag === release.tag);
  if (!record) { note.append(el('p', '此版本说明尚未整理。')); return note; }
  if (record.history_status === 'insufficient') note.append(el('p', '只有提交链接，资料不足，暂不能生成升级差异。'));
  if (record.history_status === 'pending') note.append(el('p', '部分说明仍在整理，以下仅展示已审核内容。'));
  const rows = manifest.changes.filter(c => c.version === release.version && c.component !== 'service');
  if (!rows.length && record.history_status === 'no_user_changes') note.append(el('p', '本版没有新增用户可见变化，与上一版使用相同实现。'));
  for (const kind of ReleaseNotes.types) {
    const items = rows.filter(c => c.type === kind); if (!items.length) continue;
    note.append(el('h4', categories[kind]));
    const list = el('ul');
    for (const item of items) {
      const row = el('li'); row.append(el('b', item.title['zh-CN'] || '翻译待审核'), el('p', item.description['zh-CN'] || '说明待审核'));
      row.append(el('small', item.scope.models.map(x => x.toUpperCase()).join('、') + ' · ' + item.scope.variants.map(v => variantLabels[v] || v).join(' / ')));
      list.append(row);
    }
    note.append(list);
  }
  return note;
}
function releaseCard(release, latest, snapshot) {
  const box = el(latest ? 'article' : 'details'); box.className = 'release';
  const head = el(latest ? 'h3' : 'summary', release.tag); box.append(head);
  if (latest) { const badge = el('span', '最新版'); badge.className = 'tag'; head.append(badge); }
  const table = el('table'); table.className = 'downloads';
  const tr = el('tr'); for (const text of ['型号', '固件下载']) tr.append(el('th', text)); table.append(tr);
  for (const [model, original] of Object.entries(release.models)) {
    for (const [variant, value] of [['full', original], ...Object.entries(original.variants || {})]) {
      const row = el('tr'); row.append(el('td', model.toUpperCase() + ' · ' + (variantLabels[variant] || variant)));
      const cell = el('td');
      for (const [kind, label] of [['signed', '签名 OTA'], ['raw', '原始 OTA'], ['full', '完整镜像']]) if (value.downloads[kind]) cell.append(link(label, value.downloads[kind].path, snapshot.source));
      row.append(cell); table.append(row);
    }
  }
  box.append(table, link('SHA256 校验文件', `/releases/${release.tag}/SHA256SUMS`, snapshot.source),
    link('原始单版本说明 ↗', release.notes, snapshot.source), reviewedNotes(release, snapshot.manifest));
  return box;
}
async function loadCatalog() {
  const epoch = ++generation; active?.abort(); active = new AbortController();
  document.getElementById('latest').replaceChildren(); document.getElementById('archive').replaceChildren();
  status.textContent = '正在校验发布说明与版本归档…';
  try {
    const snapshot = await ReleaseNotesClient.snapshot(source.value, {signal: active.signal});
    if (epoch !== generation) return;
    const releases = snapshot.catalog.releases;
    const stable = releases.find(r => r.tag === snapshot.catalog.stable);
    if (stable) document.getElementById('latest').append(releaseCard(stable, true, snapshot));
    else throw Error('正式最新版不在当前归档中。');
    for (const release of releases) document.getElementById('archive').append(releaseCard(release, false, snapshot));
    status.textContent = `共 ${releases.length} 个版本 · 展示已审核用户说明 · 历史文件按版本保留`;
  } catch (error) {
    if (epoch !== generation || error.code === 'REQUEST_ABORTED') return;
    status.textContent = error.code === 'SOURCE_MISMATCH' ? '说明与固件目录不同步，请切换完整来源后重试。' : '暂时无法校验发布数据，请切换来源或稍后重试。';
  }
}
source.addEventListener('change', loadCatalog);
loadCatalog();
