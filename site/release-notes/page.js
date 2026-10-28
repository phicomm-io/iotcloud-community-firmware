(function (root, factory) {
  'use strict';
  const api = factory(root);
  if (typeof module === 'object' && module.exports) module.exports = api;
  else if (root.document) for (const app of root.document.querySelectorAll('[data-release-notes]')) api.mount(app);
}(typeof globalThis !== 'undefined' ? globalThis : this, function (root) {
  'use strict';
  function mount(app, dependencies = {}) {
    if (app.releaseNotesController) return app.releaseNotesController;
    const doc = app.ownerDocument, win = doc.defaultView;
    const Notes = dependencies.Notes || root.ReleaseNotes, Client = dependencies.Client || root.ReleaseNotesClient;
    const english = app.dataset.locale === 'en', locale = english ? 'en' : 'zh-CN';
    const t = (zh, en) => english ? en : zh;
    const get = id => app.querySelector('#notes-' + id);
    const fields = Object.fromEntries(['channel', 'source', 'model', 'variant', 'from', 'to'].map(k => [k, get(k)]));
    const status = get('status'), resultBox = get('result'), submit = get('submit'), share = get('share');
    const readButton = get('read-device'), deviceStatus = get('device-status');
    const candidateBox = get('candidates'), candidateDownloads = get('candidate-downloads'), candidateStatus = get('candidate-status');
    const stableSourceOptions = [...fields.source.options].map(option => option.cloneNode(true));
    const params = new URL(win.location.href).searchParams;
    const initial = Object.fromEntries(['from', 'to', 'model', 'variant'].map(k => [k, params.get(k)]));
    fields.channel.disabled = false;
    fields.channel.value = params.get('channel') === 'candidate' ? 'candidate' : 'stable';
    const labels = english ? {feature: 'New features', improvement: 'Improvements', bugfix: 'Bug fixes', security: 'Security updates', breaking: 'Important changes'} :
      {feature: '新功能', improvement: '功能改进', bugfix: '问题修复', security: '安全更新', breaking: '重要变化'};
    const variantLabel = variant => ({full: t('完整版', 'Full'), 'no-frontend-resources': t('精简版（保留配网页）', 'Minimal (setup pages retained)'),
      'fresh-air': t('新风完整版', 'Fresh-air full'), 'fresh-air-no-frontend-resources': t('新风精简版', 'Fresh-air minimal'),
      hub: t('中枢版', 'Hub')}[variant] || variant);
    const removers = [];
    function on(node, event, handler) { node.addEventListener(event, handler); removers.push(() => node.removeEventListener(event, handler)); }
    function element(tag, text, className) {
      const el = doc.createElement(tag); if (text != null) el.textContent = text; if (className) el.className = className; return el;
    }
    let snapshot = null, sourceEpoch = 0, conditionEpoch = 0, sourceController = null, deviceController = null;
    let deviceVersion = null, firstLoad = true, destroyed = false, stableSource = fields.source.value;
    const candidateSelected = () => fields.channel.value === 'candidate';
    function scopedReleases(targetsOnly = false) {
      const latest = snapshot.manifest.releases.filter(release => release.branch === 'main' && release.channel === 'candidate')
        .sort((a, b) => Notes.compareVersions(b.version, a.version))[0]?.version;
      return snapshot.manifest.releases.filter(release => release.branch === 'main' &&
        ((release.channel === fields.channel.value && (!candidateSelected() || release.version === latest)) ||
          (!targetsOnly && candidateSelected() && release.channel === 'stable')));
    }
    function configureSource() {
      const candidate = candidateSelected();
      if (candidate && !fields.source.disabled) stableSource = fields.source.value;
      fields.source.replaceChildren(...stableSourceOptions.map(option => option.cloneNode(true)));
      if (candidate) {
        fields.source.replaceChildren(new win.Option(t('本站候选归档（GitHub）', 'This candidate archive (GitHub)'), ''));
        fields.source.value = '';
      } else fields.source.value = stableSource;
      fields.source.disabled = candidate;
      candidateBox.hidden = !candidate; candidateDownloads.replaceChildren(); candidateStatus.textContent = '';
      app.dataset.channel = fields.channel.value;
      for (const history of app.querySelectorAll('[data-notes-channel]')) {
        history.hidden = history.dataset.notesChannel !== fields.channel.value;
      }
    }
    function candidateLink(asset, release) {
      if (!asset || !Number.isSafeInteger(asset.size) || asset.size < 1 || !/^[a-f0-9]{64}$/.test(asset.sha256)) return null;
      const prefix = '/releases/' + release.tag + '/';
      if (typeof asset.path !== 'string' || !asset.path.startsWith(prefix) || !/^[A-Za-z0-9][A-Za-z0-9._-]*$/.test(asset.path.slice(prefix.length))) return null;
      return asset.path;
    }
    function renderCandidates() {
      const releases = snapshot.catalog.releases.filter(release => release.channel === 'candidate' && release.prerelease === true &&
        release.tag === 'v' + release.version && Notes.parseVersion(release.version).prerelease)
        .sort((a, b) => Notes.compareVersions(b.version, a.version)).slice(0, 1);
      if (!releases.length) throw {code: 'CANDIDATE_UNAVAILABLE', reason: 'No release candidate is available in this archive.'};
      for (const [model, label] of [['m1', t('M1 空气检测仪', 'M1 air quality monitor')], ['a1', t('A1 空气净化器', 'A1 air purifier')],
        ['tc1', t('TC1 智能排插', 'TC1 power strip')], ['daikin', t('DAIKIN 大金检测仪', 'DAIKIN air quality monitor')]]) {
        const group = element('article', null, 'notes-change'); group.id = 'notes-candidate-' + model;
        group.append(element('h3', label));
        const guide = element('a', t('功能与使用指南', 'Operation guide')); guide.href = '/guides/' + (english ? 'en/' : '') + model + '-guide.html';
        const demo = element('a', t('界面演示', 'Interface demo')); demo.href = '/demos/' + model + '/';
        const actions = element('p', null, 'notes-actions'); actions.append(guide, demo); group.append(actions);
        let available = false;
        for (const release of releases) {
          const item = release.models[model]; if (!item) continue;
          const list = element('ul');
          for (const [variant, entry] of [['full', item], ...Object.entries(item.variants || {})]) {
            if (!candidateLink(entry.downloads?.signed, release)) continue;
            const row = element('li'); row.append(element('b', variantLabel(variant)));
            for (const [kind, label] of [['signed', t('签名 OTA', 'Signed OTA')], ['raw', t('原始 OTA', 'Raw OTA')], ['full', t('完整镜像', 'Full image')]]) {
              const href = candidateLink(entry.downloads[kind], release); if (!href) continue;
              const link = element('a', label); link.href = href; row.append(doc.createTextNode(' · '), link);
            }
            list.append(row);
          }
          if (!list.childElementCount) continue;
          available = true; group.append(element('h4', release.tag + ' · Beta (RC)'));
          group.append(list);
          const checksum = element('a', t('SHA256 校验文件', 'SHA256 checksums')); checksum.href = '/releases/' + release.tag + '/SHA256SUMS';
          const links = element('p', null, 'notes-actions'); links.append(checksum);
          if (release.notes === '/releases/' + release.tag + '/notes.md') {
            const notes = element('a', t('完整累计发布说明', 'Complete cumulative notes (Chinese original)')); notes.href = release.notes; links.append(notes);
          }
          const github = 'https://github.com/phicomm-io/iotcloud-community-firmware/releases/tag/' + release.tag;
          if (release.github_release === github) { const link = element('a', t('GitHub 候选版', 'GitHub prerelease')); link.href = github; links.append(link); }
          group.append(links);
        }
        if (!available) group.append(element('p', t('暂无此型号的候选版文件。', 'No candidate files are available for this model.')));
        candidateDownloads.append(group);
      }
      candidateStatus.textContent = t('RC 实际属于 Beta 测试版本，后续仍会有较大改动，不建议使用。稳定版仍为 ',
        'RC releases are beta builds. Major changes are still expected; using them is not recommended. The stable release remains ') + snapshot.catalog.stable + (english ? '.' : '。');
    }
    function clearResult() { resultBox.replaceChildren(); share.hidden = true; app.removeAttribute('data-report-state'); }
    function invalidate() {
      conditionEpoch++; deviceController?.abort(); deviceController = null; readButton.disabled = false;
      clearResult(); deviceStatus.textContent = '';
      if (snapshot) status.textContent = t('条件已更新，请重新查询。', 'Selection changed. Compare again when ready.');
    }
    function errorText(error) {
      const zh = {
        HARDWARE_REQUIRED: '此范围含硬件专用说明，需先确认硬件版本。', INVALID_VERSION: '请选择有效的固件版本。', UNKNOWN_BUILD: '此开发版或提交后缀未能匹配已发布源码，不能当作正式版计算。',
        UNKNOWN_SOURCE: '当前版本不在正式归档中。', UNKNOWN_TARGET: '目标版本不在正式归档中。', DOWNGRADE: '当前选择是降级，不提供升级差异。',
        TARGET_NOT_FOUND: '该版本没有此型号或固件类型的归档。', SCOPE_REQUIRED: '请选择型号和固件类型。', UNSUPPORTED_SCOPE: '此分支或渠道没有已审核数据。',
        WITHDRAWN_CANDIDATE: '此旧 RC 已撤下展示，请确认最新 Beta 版本后重新查询。',
        REVIEW_REQUIRED: '相关版本仍有记录待审核，暂不提供完整差异。', LANGUAGE_MISSING: '相关说明缺少已审核的对应语言，暂不提供完整差异。',
        INCOMPLETE_SUMMARY: '连续修订缺少完整审核说明，暂不提供完整差异。', BASELINE_UNKNOWN: '无法确认当前版本的功能状态。',
        COVERAGE_UNKNOWN: '无法确认该设备范围的说明覆盖情况。', BROKEN_RELEASE_CHAIN: '发布链不完整，不能推断中间版本。',
        ARCHIVE_MISMATCH: '说明与固件归档的提交、文件或校验值不一致。', FRONTEND_IDENTITY_MISMATCH: '无法核实该固件的界面版本。',
        SOURCE_MISMATCH: '说明资源与固件目录不同步，请切换完整来源后重试。', SOURCE_UNAVAILABLE: '说明来源暂时不可用，请切换来源或稍后重试。',
        CANDIDATE_UNAVAILABLE: '本站尚未提供候选版，请查看稳定版。', INVALID_SOURCE: '候选版仅使用本站归档，请选择有效的发布来源。', INVALID_INDEX: '发布数据索引不完整，暂不能提供下载或比较。',
        NETWORK_BLOCKED: '请求未能完成，可能受到网络、CORS 或浏览器本地网络权限限制；仍可手动选择。',
        LOCAL_NETWORK_DENIED: '浏览器拒绝本地网络访问；请手动选择或调整浏览器权限。', TIMEOUT: '请求超时，仍可手动选择。',
        DEVICE_UNAUTHORIZED: '网页管理密码不正确。', DEVICE_BUSY: '设备繁忙，请稍后重试或手动选择。', OLD_DEVICE_API: '此旧接口不支持读取所需信息，请手动选择。',
        DEVICE_IDENTITY_INCOMPLETE: '接口没有完整的固件版本、型号和类型；协议 v1/v2 不能作为固件版本。请手动选择。',
        INVALID_DEVICE_ADDRESS: '请输入内网 IP 或 .local 地址，不要包含密码、路径或参数。',
        INTEGRITY_UNAVAILABLE: '浏览器无法校验发布数据，请通过 HTTPS 官网访问。',
        MIGRATION_REQUIRED: '首次安装或原厂固件请先阅读迁移指南，不生成社区固件差异。'
      };
      return (english ? error.reason || error.message : zh[error.code]) || t('发布数据不完整，暂不能提供完整差异。', 'The release data is incomplete. A full comparison is unavailable.');
    }
    function showError(error, node = status) { node.textContent = errorText(error) + (error.code ? ' [' + error.code + ']' : ''); }
    function option(select, value, label) { select.append(new win.Option(label, value)); }
    function setOptions(select, values, selected, label) {
      select.replaceChildren(); for (const value of values) option(select, value, label(value));
      select.value = values.includes(selected) ? selected : values[0] || '';
    }
    function supported() {
      return scopedReleases().filter(r => r.targets.some(target => target.model === fields.model.value && target.variant === fields.variant.value))
        .sort((a, b) => Notes.compareVersions(b.version, a.version));
    }
    function versions(fromValue = fields.from.value, toValue = fields.to.value) {
      const releases = supported(), values = releases.map(r => r.version);
      fields.from.replaceChildren(); option(fields.from, '', t('选择当前版本', 'Choose current version'));
      option(fields.from, 'stock', t('原厂固件 / 首次安装', 'Stock firmware / first installation'));
      for (const value of values) option(fields.from, value, 'v' + value);
      const original = deviceVersion || fromValue || (candidateSelected() && values.includes(snapshot.manifest.stable) ? snapshot.manifest.stable : '');
      if (original && !['stock', ...values].includes(original)) option(fields.from, original, original);
      fields.from.value = original || '';
      const targets = releases.filter(release => release.channel === fields.channel.value).map(release => release.version);
      setOptions(fields.to, targets, targets.includes(toValue) ? toValue : targets.includes(snapshot.manifest.stable) ? snapshot.manifest.stable : targets[0], v => 'v' + v);
      if (toValue && !targets.includes(toValue) && !candidateSelected()) { option(fields.to, toValue, toValue); fields.to.value = toValue; }
    }
    function variants(wanted = fields.variant.value, fromValue, toValue) {
      const values = [...new Set(scopedReleases(true).flatMap(r => r.targets.filter(x => x.model === fields.model.value).map(x => x.variant)))];
      setOptions(fields.variant, values, wanted || 'full', variantLabel); versions(fromValue, toValue);
    }
    function selectTab(which) {
      for (const name of ['history', 'compare']) {
        const selected = name === which;
        get(name + '-tab').setAttribute('aria-selected', String(selected)); get(name + '-tab').tabIndex = selected ? 0 : -1;
      }
      get('history').hidden = which !== 'history'; get('comparison').hidden = which !== 'compare';
    }
    for (const name of ['history', 'compare']) {
      on(get(name + '-tab'), 'click', () => selectTab(name));
      on(get(name + '-tab'), 'keydown', event => {
        if (!['ArrowLeft', 'ArrowRight', 'Home', 'End'].includes(event.key)) return;
        event.preventDefault(); const next = event.key === 'Home' ? 'history' : event.key === 'End' ? 'compare' : name === 'history' ? 'compare' : 'history';
        selectTab(next); get(next + '-tab').focus();
      });
    }
    function comparison() {
      clearResult(); if (!snapshot) return;
      if (!fields.from.value) { status.textContent = t('请选择当前固件版本。', 'Choose the current firmware version.'); return; }
      const query = {from: fields.from.value, to: fields.to.value, model: fields.model.value, variant: fields.variant.value, branch: 'main', channel: fields.channel.value, locale};
      const result = Notes.compare(snapshot.manifest, query, snapshot.catalog);
      if (!result.ok) {
        app.dataset.reportState = 'error'; showError(result.error);
        if (result.error.code === 'MIGRATION_REQUIRED') {
          const link = element('a', t('阅读安装 / 迁移指南', 'Read installation / migration guide'));
          link.href = result.error.details.guide; resultBox.append(link);
        }
        return;
      }
      app.dataset.reportState = 'complete';
      status.textContent = `v${result.from} → v${result.to} · ${result.scope.model.toUpperCase()} · ${variantLabel(result.scope.variant)}`;
      const notices = result.upgrade_notices.filter(n => !result.changes.some(c => c.change_id === n.change_id && c.version === n.version));
      if (notices.length) {
        const box = element('aside', null, 'notes-notices'); box.append(element('h3', t('升级提示', 'Upgrade notices')));
        for (const notice of notices) { box.append(element('b', notice.title), element('p', notice.description)); }
        resultBox.append(box);
      }
      if (!result.changes.length) resultBox.append(element('p', result.from === result.to ? t('当前版本与目标相同，没有升级差异。', 'The current and target versions are the same. There are no upgrade changes.') :
        t('此区间没有适用于该固件的用户可见变化。', 'There are no user-visible changes for this firmware in the selected range.'), 'notes-empty'));
      for (const type of Notes.types) if (result.groups[type].length) {
        const group = element('section', null, 'notes-change-group'); group.append(element('h3', labels[type]));
        for (const change of result.groups[type]) {
          const card = element('article', null, 'notes-change'); card.dataset.changeId = change.change_id;
          card.append(element('h4', change.title), element('p', change.description), element('small',
            result.scope.model.toUpperCase() + ' · ' + variantLabel(result.scope.variant) + ' · ' +
            (change.component === 'bundled_web' ? t('随固件网页', 'Bundled page') : t('固件', 'Firmware'))));
          group.append(card);
        }
        resultBox.append(group);
      }
      const identity = element('details', null, 'notes-target'); identity.append(element('summary', t('目标固件与校验信息', 'Target firmware and verification')));
      const link = element('a', result.target.path.split('/').pop()); link.href = (candidateSelected() ? '' : snapshot.source) + result.target.path; identity.append(link);
      identity.append(element('p', t('源码：', 'Source: ') + result.target.source_commit), element('p', 'SHA-256: ' + result.target.sha256));
      resultBox.append(identity);
      const url = new URL(win.location.href); url.search = ''; url.hash = '';
      for (const key of ['from', 'to', 'model', 'variant']) url.searchParams.set(key, query[key]);
      if (candidateSelected()) url.searchParams.set('channel', 'candidate');
      share.href = url.href; share.hidden = false;
      win.history.replaceState(null, '', url.pathname + url.search);
    }
    on(get('form'), 'submit', event => { event.preventDefault(); comparison(); });
    on(fields.model, 'change', () => { invalidate(); deviceVersion = null; if (snapshot) variants('full', '', ''); });
    on(fields.variant, 'change', () => { invalidate(); deviceVersion = null; if (snapshot) versions('', ''); });
    for (const field of ['from', 'to']) on(fields[field], 'change', () => { deviceVersion = null; invalidate(); });
    on(get('address'), 'input', invalidate); on(get('password'), 'input', invalidate);
    async function load() {
      invalidate(); snapshot = null; const epoch = ++sourceEpoch;
      sourceController?.abort(); sourceController = new AbortController();
      configureSource(); const source = fields.source.value, channel = fields.channel.value;
      for (const field of ['model', 'variant', 'from', 'to']) fields[field].disabled = true;
      submit.disabled = true; status.textContent = t('正在校验发布数据…', 'Verifying release data…');
      if (candidateSelected()) candidateStatus.textContent = status.textContent;
      try {
        const value = await Client.snapshot(source, {signal: sourceController.signal, channel});
        if (destroyed || epoch !== sourceEpoch) return;
        snapshot = value;
        if (candidateSelected()) renderCandidates();
        fields.channel.options[0].textContent = t('稳定版 · v', 'Stable · v') + snapshot.manifest.stable;
        const models = [...new Set(scopedReleases(true).flatMap(r => r.targets.map(x => x.model)))].sort();
        const wanted = firstLoad ? initial.model || 'a1' : fields.model.value;
        setOptions(fields.model, models, wanted, m => m.toUpperCase());
        if (wanted && !models.includes(wanted)) { option(fields.model, '', t('请选择适用型号', 'Choose a supported model')); fields.model.value = ''; }
        variants(firstLoad ? initial.variant || 'full' : fields.variant.value, firstLoad ? initial.from || '' : fields.from.value, firstLoad ? initial.to || '' : fields.to.value);
        for (const field of ['model', 'variant', 'from', 'to']) fields[field].disabled = false;
        submit.disabled = false; status.textContent = t('发布数据已校验，选择当前版本后查询。', 'Release data verified. Choose the current version to compare.');
        if (firstLoad && initial.variant && !supported().some(r => r.targets.some(x => x.model === wanted && x.variant === initial.variant))) {
          clearResult(); app.dataset.reportState = 'error'; showError({code: 'TARGET_NOT_FOUND', reason: 'The shared firmware scope is unavailable.'});
        } else if (firstLoad && candidateSelected() && initial.to && initial.to.replace(/^v/, '') !== fields.to.value) {
          clearResult(); app.dataset.reportState = 'error'; selectTab('compare');
          showError({code: 'WITHDRAWN_CANDIDATE', reason: 'This older RC has been withdrawn from display. Confirm the latest beta target and compare again.'});
        } else if (firstLoad && initial.from) { selectTab('compare'); comparison(); }
        firstLoad = false;
      } catch (error) {
        if (destroyed || epoch !== sourceEpoch || error.code === 'REQUEST_ABORTED') return;
        snapshot = null; candidateDownloads.replaceChildren();
        if (candidateSelected()) showError(error, candidateStatus);
        showError(error); app.dataset.reportState = 'error';
      }
    }
    on(fields.source, 'change', () => { stableSource = fields.source.value; load(); });
    on(fields.channel, 'change', () => {
      deviceVersion = null; fields.from.value = ''; fields.to.value = '';
      load();
    });
    on(get('device-form'), 'submit', async event => {
      event.preventDefault(); invalidate(); const epoch = conditionEpoch;
      const address = get('address').value.trim(), password = get('password').value;
      get('password').value = ''; deviceController = new AbortController(); readButton.disabled = true;
      deviceStatus.textContent = t('正在读取设备信息…', 'Reading device information…');
      try {
        const info = await Client.readDevice(address, password, {signal: deviceController.signal});
        if (destroyed || epoch !== conditionEpoch) return;
        if (!snapshot) throw {code: 'SOURCE_UNAVAILABLE', reason: 'Release data has not loaded.'};
        const matching = snapshot.manifest.releases.some(r => r.targets.some(x => x.model === info.model && x.variant === info.variant));
        if (!matching) throw {code: 'TARGET_NOT_FOUND', reason: 'This model or variant has no published archive.'};
        fields.model.value = info.model; deviceVersion = info.version; variants(info.variant, info.version, '');
        deviceStatus.textContent = `${info.model.toUpperCase()} · ${variantLabel(info.variant)} · ${info.version}`;
        status.textContent = t('已读取设备信息，请查看升级差异。', 'Device information read. Compare when ready.');
      } catch (error) {
        if (destroyed || epoch !== conditionEpoch || error.code === 'REQUEST_ABORTED') return;
        showError(error, deviceStatus);
      } finally { if (epoch === conditionEpoch) { readButton.disabled = false; deviceController = null; } }
    });
    const controller = {ready: null, destroy() {
      destroyed = true; sourceEpoch++; invalidate(); sourceController?.abort(); for (const remove of removers) remove();
      delete app.releaseNotesController;
    }};
    if (params.get('view') === 'compare') selectTab('compare');
    app.releaseNotesController = controller; controller.ready = load(); return controller;
  }
  return Object.freeze({mount});
}));
