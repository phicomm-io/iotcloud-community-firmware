#!/usr/bin/env python3
"""Exercise signed OTA from Pages through bounded Range responses."""
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import subprocess
import urllib.request

root = Path(__file__).resolve().parents[2]
work = root / 'build/tls-probe'
with urllib.request.urlopen('https://phicomm-io.github.io/releases/index.json', timeout=30) as response:
    catalog = json.load(response)
(work / 'catalog.json').write_text(json.dumps(catalog, indent=2) + '\n')
release = catalog['releases'][0]


def download(model, item):
    image = item['downloads']['signed']
    output = work / f'{model}-verified.raw'
    process = subprocess.run([str(work / 'client'), 'phicomm-io.github.io',
                              image['path'], '2048', '1024', str({'tc1': 1, 'm1': 2, 'a1': 3}[model]),
                              str(output)], text=True, capture_output=True, timeout=200)
    digest = hashlib.sha256(output.read_bytes()).hexdigest() if output.exists() else None
    expected = item['downloads']['raw']['sha256']
    result = dict(model=model, tag=release['tag'], buffer=2048, range=1024,
                  exit=process.returncode, metrics=process.stdout.strip(),
                  stderr=process.stderr, sha256=digest, expected_sha256=expected,
                  match=process.returncode == 0 and digest == expected)
    print(json.dumps(result), flush=True)
    return result

with ThreadPoolExecutor(max_workers=3) as pool:
    futures = [pool.submit(download, model, item) for model, item in release['models'].items()]
    results = [future.result() for future in futures]
(work / 'results-signed.json').write_text(json.dumps(results, indent=2) + '\n')
# Public signed fixture for the local transport/fault tests; verify exact catalog bytes.
image = release['models']['a1']['downloads']['signed']
with urllib.request.urlopen('https://phicomm-io.github.io' + image['path'], timeout=30) as response:
    data = response.read()
assert len(data) == image['size'] and hashlib.sha256(data).hexdigest() == image['sha256']
(work / 'a1.signed').write_bytes(data)
if any(not result['match'] for result in results):
    raise SystemExit('Signed OTA verification failed')
