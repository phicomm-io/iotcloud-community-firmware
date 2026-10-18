from pathlib import Path
import subprocess, tempfile, sys, re
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "common/web"))
from page import render_page
for product in ('A1', 'M1', 'TC1'):
    html = render_page(Path(product) / ('http_server/web/index.html' if product == 'TC1' else 'web/index.html')).decode()
    scripts = html.split('<script>')[1:]
    for source in scripts:
        source = source.split('</script>')[0]
        with tempfile.NamedTemporaryFile(suffix='.js', mode='w') as f:
            f.write(source)
            f.flush()
            subprocess.run(['node', '--check', f.name], check=True)
    if product in ('M1','A1'):
        server=(Path(product)/'http.c').read_text()
        assert re.search(r'sizeof\(' + product.lower() + r'_web_data\),true[,)]', server), 'compressed HTML requires Content-Encoding: gzip'
    # Every page carries the shared framework, the theme bootstrap and the
    # settings pages the product advertises.
    for required in ('phicomm.appearance', 'window.UI', 'window.I18N', 'window.automationPages'):
        assert required in html, f'{product}: {required} missing'
    # Product-specific markup: the A1 has light effects and M1 binding, the
    # M1 has no power control, the TC1 has socket tiles.
    if product == 'A1':
        assert 'id="power"' in html and 'id="ring"' in html
    if product == 'M1':
        assert 'id="metrics"' in html and 'id="power"' not in html and 'id="tiles"' not in html
    if product == 'TC1':
        assert 'id="tiles"' in html and 'id="socketNamesForm"' in html and 'id="power"' not in html
print('TC1, A1 and M1 embedded WebUI JavaScript syntax passed')
