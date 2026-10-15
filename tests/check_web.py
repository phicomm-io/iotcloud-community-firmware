from pathlib import Path
import subprocess, tempfile, sys
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
    assert 'phicomm.appearance' in html
    if product == 'A1': assert 'featureForm' in html and 'response_ids' in html
    if product == 'M1': assert 'responseForm' in html and 'featureForm' not in html
print('TC1, A1 and M1 embedded WebUI JavaScript syntax passed')
