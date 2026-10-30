"""Accessible static history with an optional browser comparison form."""
from pathlib import Path
import markdown
from user_notes import history_html


def body(manifest, locale):
    english = locale == 'en'
    def t(zh, en):
        return en if english else zh

    overview_source = Path(__file__).resolve().parents[1] / 'docs/release-notes' / f'v1-overview.{locale}.md'
    overview = markdown.markdown(overview_source.read_text(), extensions=['tables', 'fenced_code'])
    beta = f'''<section class="notes-history-version" data-notes-channel="candidate" id="v1.0.0-rc.4">
<h2><time datetime="2026-10-30">2026-10-30</time></h2><h3>v1.0.0-rc.4 · Beta</h3>
<details class="notes-overview" id="v1-overview" open><summary>{t('v1 Beta 累计功能（相对 v0.9.27）', 'v1 Beta cumulative changes since v0.9.27')}</summary>{overview}</details></section>'''

    return f'''<div class="notes-app" data-release-notes data-locale="{locale}">
<p class="eyebrow">{t('发布与升级', 'RELEASES & UPGRADES')}</p>
<h1>{t('更新日志','Release Log')}</h1>
<p>{t('按日期和版本查阅更新，也可以比较两个固件版本的变化。', 'Browse changes by date and release, or compare two firmware versions.')}</p>
<p><a class="btn" href="{'../../en/' if english else '../'}#releases">{t('固件下载', 'Firmware downloads')}</a></p>
<div class="notes-fields"><label for="notes-channel">{t('发布渠道', 'Release channel')}<select id="notes-channel" disabled><option value="stable">{t('稳定版', 'Stable releases')}</option><option value="candidate">{t('Beta（RC）', 'Beta (RC)')}</option></select></label></div>
<div class="notes-tabs" role="tablist" aria-label="{t('查看方式', 'View')}">
<button type="button" role="tab" id="notes-history-tab" aria-controls="notes-history" aria-selected="true">{t('版本历史', 'Version history')}</button>
<button type="button" role="tab" id="notes-compare-tab" aria-controls="notes-comparison" aria-selected="false">{t('升级差异', 'Upgrade comparison')}</button>
</div>
<section id="notes-candidates" aria-labelledby="notes-candidates-title" hidden>
<h2 id="notes-candidates-title">{t('Beta 下载（RC）', 'Beta downloads (RC)')}</h2>
<p>{t('RC 用于 Beta 测试。请按设备型号和固件类型选择。', 'RC builds are for beta testing. Choose your device model and firmware variant.')}</p>
<p id="notes-candidate-status" role="status" aria-live="polite"></p>
<div id="notes-candidate-downloads"></div>
</section>
<section id="notes-comparison" role="tabpanel" aria-labelledby="notes-compare-tab" hidden>
<h2>{t('这次升级，有哪些变化？', 'What changes in this upgrade?')}</h2>
<p class="muted">{t('选择设备和版本。查询仅在浏览器中计算，不会检查或安装设备更新。', 'Choose a device and releases. Comparisons run in your browser and do not check for or install device updates.')}</p>
<form id="notes-form">
<div class="notes-fields">
<label for="notes-source">{t('说明与下载来源', 'Notes and download source')}<select id="notes-source"><option value="">{t('本站归档（GitHub）', 'This archive (GitHub)')}</option><option value="https://phicomm.fjrcn.com">{t('境外服务器', 'Global server')}</option><option value="https://phicomm.fjrcn.cn">{t('境内镜像', 'China mirror')}</option></select></label>
<label for="notes-model">{t('设备型号', 'Device model')}<select id="notes-model" required disabled></select></label>
<label for="notes-variant">{t('固件类型', 'Firmware variant')}<select id="notes-variant" required disabled></select></label>
<label for="notes-from">{t('当前固件版本', 'Current firmware version')}<select id="notes-from" required disabled></select></label>
<label for="notes-to">{t('目标版本', 'Target release')}<select id="notes-to" required disabled></select></label>
</div>
<div class="notes-actions"><button type="submit" class="btn primary" id="notes-submit" disabled>{t('查看升级差异', 'Compare upgrade')}</button><a id="notes-share" hidden>{t('分享此查询', 'Share this comparison')}</a></div>
</form>
<details class="notes-device"><summary>{t('可选：读取设备信息', 'Optional: read device information')}</summary>
<p>{t('设备与浏览器需在可互通的内网。无法读取时可继续手动选择；网页密码只用于本次请求。', 'The device must be reachable on your local network. Manual selection is always available; the web password is used only for this request.')}</p>
<form id="notes-device-form"><div class="notes-fields"><label for="notes-address">{t('设备内网地址', 'Device LAN address')}<input id="notes-address" placeholder="192.168.1.100" autocomplete="off" inputmode="url" required></label>
<label for="notes-password">{t('网页管理密码', 'Web management password')}<input id="notes-password" type="password" autocomplete="off"></label></div>
<button type="submit" class="btn" id="notes-read-device">{t('读取一次', 'Read once')}</button></form>
<p id="notes-device-status" role="status" aria-live="polite"></p></details>
<p id="notes-status" role="status" aria-live="polite">{t('正在校验发布数据…', 'Verifying release data…')}</p>
<div id="notes-result" aria-live="polite"></div>
<noscript><p>{t('升级区间计算需要 JavaScript；下方版本历史仍可直接阅读。', 'Upgrade comparisons require JavaScript; the release history below remains readable.')}</p></noscript>
</section>
<section id="notes-history" role="tabpanel" aria-labelledby="notes-history-tab">{beta}<div data-notes-channel="stable">{history_html(manifest, locale)}</div></section>
<noscript><p>{t('稳定版历史仍可直接阅读。候选版下载与升级比较需启用 JavaScript。', 'Stable release history remains readable. Enable JavaScript for candidate downloads and upgrade comparisons.')}</p></noscript>
</div>'''
