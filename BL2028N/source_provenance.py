"""Check the frozen shared-service/frontend source mappings before building."""
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent


def verify_sources():
    source = json.loads((ROOT / 'protocol/lab-shared-source.json').read_text())
    expected = {name: item['sha256'] for name, item in source['files'].items()}
    expected.update(source['adaptation_patch_sha256'])
    frontend = json.loads((ROOT / 'web/shared/provenance.json').read_text())
    for name, digest in frontend['sha256'].items():
        path = 'web/shared/' + name
        if path in expected and expected[path] != digest:
            raise ValueError('Conflicting source provenance: ' + path)
        expected[path] = digest
    for name, digest in expected.items():
        if hashlib.sha256((ROOT / name).read_bytes()).hexdigest() != digest:
            raise ValueError('Shared source or adaptation patch changed: ' + name)


if __name__ == '__main__':
    verify_sources()
    print('PASS: shared-service and frontend source provenance')
