from pathlib import Path
import subprocess, tempfile
for product in ('A1', 'M1'):
    html = (Path(product) / 'web/index.html').read_text()
    scripts = html.split('<script>')[1:]
    for source in scripts:
        source = source.split('</script>')[0]
        with tempfile.NamedTemporaryFile(suffix='.js', mode='w') as f:
            f.write(source)
            f.flush()
            subprocess.run(['node', '--check', f.name], check=True)
    assert 'featureForm' in html and '/binding/config' in html
print('A1 and M1 embedded WebUI JavaScript syntax passed')
