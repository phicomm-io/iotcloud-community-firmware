'use strict';
// Local UI tests with synthetic candidate metadata. Firmware files are never
// created or served; this is not a release, signature or download validation.
const assert = require('node:assert/strict'), fs = require('node:fs'), http = require('node:http'), path = require('node:path');
const {execFileSync} = require('node:child_process');
const Playwright = require(process.env.PHICOMM_PLAYWRIGHT || '@playwright/test');
const CandidateUI = require('./fixtures/release_candidate_ui.cjs');
const root = path.resolve(__dirname, '..');
function retainedSnapshot(directory) {
  if (!directory) return undefined;
  const read = filename => JSON.parse(fs.readFileSync(path.join(directory, filename)));
  const index = read('releases/notes/index.json');
  return {source: '', channel: 'stable', id: index.manifest.sha256,
    manifest: read(index.manifest.path.slice(1)), catalog: read(index.catalog.path.slice(1))};
}
async function main() {
  const f = CandidateUI.fixture(retainedSnapshot(process.env.PHICOMM_RC_BASE_ARCHIVE));
  CandidateUI.appendCandidate(f.candidate, '1.0.0-rc.1', '1.0.0-rc', 103);
  CandidateUI.appendCandidate(f.candidate, '1.0.0-rc.2', '1.0.0-rc.1', 104);
  CandidateUI.appendCandidate(f.candidate, '1.0.0-rc.4', '1.0.0-rc.2', 105);
  const nativeCatalog=fs.readFileSync(path.join(root,'site/native-downloads.json'),'utf8');
  const resources = {...CandidateUI.resources(f.stable), ...CandidateUI.resources(f.candidate)};
  const pages = JSON.parse(execFileSync(process.env.PYTHON || 'python3', ['-c', `import sys,json
from pathlib import Path
sys.path[:0]=['site','tools/release']
import build
manifest=json.load(sys.stdin)
print(json.dumps({lang:build.render_guide('release-log','Release Log','Releases',Path('docs/release/Release-Log.md'),lang,manifest) for lang in ['zh','en']}))
`], {cwd: root, input: JSON.stringify(f.stable.manifest), maxBuffer: 4e6}));
  let candidateAvailable = true;
  const server = http.createServer((request, response) => {
    const pathname = new URL(request.url, 'http://localhost').pathname;
    let body, type = 'application/json';
    if (pathname === '/guides/release-log.html' || pathname === '/guides/en/release-log.html') {
      body = pages[pathname.includes('/en/') ? 'en' : 'zh']; type = 'text/html; charset=utf-8';
    } else if(pathname==='/'||pathname==='/en/') {
      body=fs.readFileSync(path.join(root,'site',pathname==='/en/'?'index-en.html':'index.html'),'utf8').replace('</head>','<script type="application/json" id="native-downloads">'+nativeCatalog+'</script></head>');type='text/html; charset=utf-8';
    } else if (pathname === '/releases/notes/candidate/index.json' && !candidateAvailable) body = undefined;
    else if (resources[pathname]) body = resources[pathname];
    else if (['/style.css', '/theme.js', '/site.js','/releases.js'].includes(pathname)) {
      body = fs.readFileSync(path.join(root, 'site', pathname.slice(1))); type = pathname.endsWith('.css') ? 'text/css' : 'text/javascript';
    } else if (/^\/releases\/notes\/(core|client|page)\.js$/.test(pathname)) {
      body = fs.readFileSync(path.join(root, 'site/release-notes', path.basename(pathname))); type = 'text/javascript';
    }
    response.writeHead(body === undefined ? 404 : 200, {'content-type': type, 'cache-control': 'no-store'});
    response.end(body === undefined ? 'No firmware assets are served by this UI fixture.' : body);
  });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const base = 'http://127.0.0.1:' + server.address().port;
  const out = process.env.PHICOMM_RC_EVIDENCE_DIR || path.join(root, 'build/rc-ui-evidence'); fs.mkdirSync(out, {recursive: true});
  const report = {status: 'PASS',scope: 'LOCAL SYNTHETIC CANDIDATE UI ONLY', firmwareAssetsServed: false, physicalDeviceAccess: false,
    stableHistoryCount: f.stable.manifest.releases.length, cases: [], noJavaScript: []};
  try {
    for (const name of (process.env.PHICOMM_RC_BROWSERS || 'chromium').split(',')) {
      const engine = Playwright[name]; assert.ok(engine, 'Unknown browser: ' + name);
      const browser = await engine.launch(name === 'webkit' && process.env.PHICOMM_WEBKIT_EXECUTABLE_PATH ? {executablePath: process.env.PHICOMM_WEBKIT_EXECUTABLE_PATH} : {});
      try {
        for (const locale of ['zh', 'en']) for (const width of [390, 1440]) {
          const context = await browser.newContext({viewport: {width, height: 1000}, locale: locale === 'zh' ? 'zh-CN' : 'en', serviceWorkers: 'block'});
          try {
            const page = await context.newPage(), errors = [], calls = [];
            await page.route('https://api.github.com/**',route=>route.fulfill({contentType:'application/json',headers:{'access-control-allow-origin':'*'},body:'[]'}));
            page.on('pageerror', error => errors.push(error.message)); page.on('request', request => calls.push(new URL(request.url()).pathname));
            const route = '/guides/' + (locale === 'en' ? 'en/' : '') + 'release-log.html';
            await page.goto(base + route + '?view=compare&model=m1');
            await Playwright.expect(page.locator('#notes-to')).toHaveValue('0.9.27');
            await Playwright.expect(page.locator('#notes-channel')).toHaveValue('stable');
            await Playwright.expect(page.locator('#notes-candidates')).toBeHidden();
            await Playwright.expect(page.locator('[data-notes-channel=stable] .notes-history-version')).toHaveCount(f.stable.manifest.releases.length);
            assert.ok(calls.every(value => !value.includes('/candidate/')));
            const history = await page.locator('[data-notes-channel=stable]').innerHTML();
            await page.locator('#notes-channel').selectOption('candidate');
            await Playwright.expect(page.locator('#notes-candidate-downloads article')).toHaveCount(4);
            await Playwright.expect(page.locator('#notes-from')).toHaveValue('0.9.27');
            await Playwright.expect(page.locator('#notes-to')).toHaveValue('1.0.0-rc.4');
            await Playwright.expect(page.locator('#notes-to option')).toHaveCount(1);
            await Playwright.expect(page.locator('#notes-candidate-status')).toContainText(locale === 'en' ? 'beta testing' : 'Beta 测试');
            await Playwright.expect(page.locator('#notes-candidate-status')).toContainText('v0.9.27');
            await Playwright.expect(page.locator('#notes-source option')).toHaveCount(1);
            await Playwright.expect(page.locator('#notes-candidate-downloads a[href$=".ota.bin.signed"]')).toHaveCount(10);
            const links = await page.locator('#notes-candidate-downloads a').evaluateAll(nodes => nodes.map(node => node.getAttribute('href')));
            assert.ok(links.every(value => value.startsWith('/') || value.startsWith('https://github.com/phicomm-io/iotcloud-community-firmware/releases/tag/')));
            assert.ok(links.filter(value => value.startsWith('/releases/')).every(value => value.includes('/v1.0.0-rc.4/')));
            for (const model of ['m1', 'a1', 'tc1', 'daikin']) {
              await page.locator('#notes-model').selectOption(model); await page.locator('#notes-form').evaluate(form => form.requestSubmit());
              await Playwright.expect(page.locator('[data-release-notes]')).toHaveAttribute('data-report-state', 'complete');
              await Playwright.expect(page.locator('.notes-change-group .notes-change')).toHaveCount(1);
            }
            assert.equal(await page.locator('[data-notes-channel=stable]').innerHTML(), history);
            const share = await page.locator('#notes-share').getAttribute('href'); assert.equal(new URL(share).searchParams.get('channel'), 'candidate');
            assert.equal(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), true);
            await page.screenshot({path: path.join(out, `candidate-${name}-${locale}-${width}.png`), fullPage: true});
            await page.locator('#notes-channel').selectOption('stable'); await Playwright.expect(page.locator('#notes-to')).toHaveValue('0.9.27');
            await Playwright.expect(page.locator('#notes-candidate-downloads')).toBeEmpty(); await Playwright.expect(page.locator('#notes-share')).toBeHidden();
            await page.locator('#notes-history-tab').click();
            assert.equal(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), true);
            await page.screenshot({path: path.join(out, `stable-${name}-${locale}-${width}.png`)});
            await page.goto(share); await Playwright.expect(page.locator('[data-release-notes]')).toHaveAttribute('data-report-state', 'complete');
            await Playwright.expect(page.locator('#notes-channel')).toHaveValue('candidate');
            await page.locator('#notes-channel').selectOption('stable'); await Playwright.expect(page.locator('#notes-to')).toHaveValue('0.9.27');
            candidateAvailable = false;
            const before = calls.length; await page.locator('#notes-channel').selectOption('candidate');
            await Playwright.expect(page.locator('[data-release-notes]')).toHaveAttribute('data-report-state', 'error');
            await Playwright.expect(page.locator('#notes-candidate-downloads')).toBeEmpty();
            assert.deepEqual(calls.slice(before), ['/releases/notes/candidate/index.json']); candidateAvailable = true;
            await page.goto(base+(locale==='en'?'/en/':'/'));
            const downloads=page.locator('#candidate-releases');
            await Playwright.expect(downloads.locator('[data-download-model]')).toHaveCount(5);
            for(const model of ['m1','a1','tc1','s30','daikin']){
              const card=downloads.locator('[data-download-model="'+model+'"]'),button=card.locator(':scope > button'),dialog=card.locator('dialog');
              await button.click();await Playwright.expect(dialog).toBeVisible();
              assert.ok(await dialog.locator('.firmware-file').count()>=2);
              assert.ok(await dialog.evaluate(node=>node.scrollWidth<=node.clientWidth));
              if(model==='s30'){
                await Playwright.expect(dialog.locator('.firmware-file')).toHaveCount(2);
                await Playwright.expect(dialog).toContainText('0x11000');
                await page.screenshot({path:path.join(out,`downloads-${name}-${locale}-${width}.png`)});
              }
              await page.keyboard.press('Escape');await Playwright.expect(dialog).toBeHidden();
              await Playwright.expect(button).toBeFocused();
            }
            assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth),true);
            assert.deepEqual(errors, []);
            report.cases.push({browser: name, locale, width, stableDefault: true, explicitCandidateOnly: true, stableBaseline: '0.9.27', deviceGroups: 4,
              syntheticSignedTargets: 10, candidateComparisonAndShare: true, stableHistoryUnchanged: true, unavailableCandidateFailsClosed: true, overflow: false});
          } finally {candidateAvailable = true; await context.close();}
        }
        for (const locale of ['zh', 'en']) {
          const context = await browser.newContext({viewport: {width: 390, height: 1000}, javaScriptEnabled: false});
          try {
            const page = await context.newPage();
            await page.goto(base + '/guides/' + (locale === 'en' ? 'en/' : '') + 'release-log.html');
            await Playwright.expect(page.locator('#notes-history')).toBeVisible();
            await Playwright.expect(page.locator('[data-notes-channel=stable] .notes-history-version')).toHaveCount(f.stable.manifest.releases.length);
            await Playwright.expect(page.locator('#notes-channel')).toBeDisabled();
            await Playwright.expect(page.locator('#notes-channel')).toHaveValue('stable');
            await Playwright.expect(page.locator('#notes-candidates')).toBeHidden();
            assert.equal(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), true);
            await page.screenshot({path: path.join(out, `nojs-${name}-${locale}-390.png`)});
            report.noJavaScript.push({browser: name, locale, width: 390, stableHistoryCount: f.stable.manifest.releases.length, candidateUnavailable: true, overflow: false});
          } finally {await context.close();}
        }
      } finally {await browser.close();}
    }
    fs.writeFileSync(path.join(out, 'browser.json'), JSON.stringify(report, null, 2) + '\n');
    process.stdout.write(report.cases.length + ' candidate/stable UI browser scenarios and ' + report.noJavaScript.length + ' no-JavaScript scenarios passed\n');
  } finally {await new Promise(resolve => server.close(resolve));}
}
main().catch(error => {console.error(error); process.exitCode = 1;});
