'use strict';
// Synthetic metadata for UI isolation tests only. No firmware asset is created
// or served, and these records must never enter the public release archive.
const {createHash} = require('node:crypto');
const clone = value => structuredClone(value);
const hash = bytes => createHash('sha256').update(bytes).digest('hex');
const words = text => ({'zh-CN': text, en: 'EN ' + text});
const variants = {m1: ['full', 'no-frontend-resources'], a1: ['full', 'no-frontend-resources', 'fresh-air', 'fresh-air-no-frontend-resources'],
  tc1: ['full', 'no-frontend-resources'], daikin: ['full', 'hub']};
function release(version, channel, previous, sequence) {
  const commit = sequence.toString(16).padStart(40, '0'), frontend = {kind: 'in-tree', source_commit: commit};
  const record = {version, tag: 'v' + version, previous, branch: 'main', channel, baseline_confirmed: true,
    history_status: channel === 'stable' ? 'no_user_changes' : 'complete', targets: [], coverage: []};
  const archive = {version, tag: 'v' + version, commit, models: {}, notes: '/releases/v' + version + '/notes.md'};
  if (channel === 'candidate') Object.assign(archive, {channel, prerelease: true, source: {scope: channel, kind: 'relative-archive'},
    github_release: 'https://github.com/phicomm-io/iotcloud-community-firmware/releases/tag/v' + version});
  for (const [model, supported] of Object.entries(variants)) for (const variant of supported) {
    if (channel === 'stable' && variant === 'hub') continue;
    const base = '/releases/v' + version + '/' + model + '-' + variant;
    const downloads = Object.fromEntries([['signed', '.ota.bin.signed'], ['raw', '.ota.bin.raw'], ['full', '.all.bin']].map(([kind, suffix]) =>
      [kind, {path: base + suffix, size: 1024, sha256: 'a'.repeat(64)}]));
    const target = {model, variant, hardware: '*', kind: 'signed', tag_commit: commit, source_commit: commit, frontend: clone(frontend), ...downloads.signed};
    record.targets.push(target); record.coverage.push({scope: {models: [model], variants: [variant], hardware: ['*'], branches: ['main'], channels: [channel]},
      status: 'complete', locales: ['zh-CN', 'en']});
    const entry = {...downloads.signed, downloads};
    if (variant === 'full') archive.models[model] = {...entry, variants: {}};
    else archive.models[model].variants[variant] = entry;
  }
  return {record, archive, source: {commit, frontend}};
}
function fixture(stableSnapshot) {
  const stableRelease = release('0.9.27', 'stable', null, 101);
  const stable = stableSnapshot ? clone(stableSnapshot) : {source: '', channel: 'stable', id: 'synthetic-stable',
    catalog: {schema: 1, stable: 'v0.9.27', releases: [stableRelease.archive]},
    manifest: {schema: 1, stable: '0.9.27', supported_scopes: [{branch: 'main', channel: 'stable'}],
      releases: [stableRelease.record], sources: {[stableRelease.source.commit]: stableRelease.source}, changes: [], merges: []}};
  const candidate = clone(stable), rc = release('1.0.0-rc', 'candidate', '0.9.27', 102);
  candidate.id = 'synthetic-candidate'; candidate.channel = 'candidate'; candidate.source = '';
  candidate.catalog.releases.push(rc.archive); candidate.manifest.releases.push(rc.record);
  candidate.manifest.supported_scopes.push({branch: 'main', channel: 'candidate'});
  candidate.manifest.sources[rc.source.commit] = rc.source;
  candidate.manifest.changes.push({change_id: 'test-only-candidate', version: '1.0.0-rc', type: 'feature', component: 'firmware',
    scope: {models: Object.keys(variants), variants: [...new Set(Object.values(variants).flat())], hardware: ['*'], branches: ['main'], channels: ['candidate']},
    review_status: 'approved', lifecycle: {state: 'active'}, summary_complete: true, title: words('候选界面测试'), description: words('仅用于本地界面隔离测试。'),
    ota_summary: words('仅用于本地界面隔离测试。')});
  return {stable, candidate};
}
function resources(snapshot, channel = snapshot.channel || 'stable') {
  const directory = '/releases/notes/' + (channel === 'candidate' ? 'candidate/' : '');
  const catalog = Buffer.from(JSON.stringify(snapshot.catalog));
  const manifest = Buffer.from(JSON.stringify({...snapshot.manifest, archive_sha256: hash(catalog)}));
  const catalogPath = channel === 'candidate' ? directory + 'catalog.' + hash(catalog) + '.json' : '/releases/index.json';
  const manifestPath = directory + 'manifest.' + hash(manifest) + '.json';
  const index = {schema: 1, catalog: {path: catalogPath, size: catalog.length, sha256: hash(catalog)},
    manifest: {path: manifestPath, size: manifest.length, sha256: hash(manifest)}};
  if (channel === 'candidate') Object.assign(index, {channel, source: {scope: channel, kind: 'relative-archive'}});
  return {[directory + 'index.json']: Buffer.from(JSON.stringify(index)), [catalogPath]: catalog, [manifestPath]: manifest};
}
module.exports = {fixture, resources, hash};
