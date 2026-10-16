"""Compatibility entrypoint: render only the commit-pinned frontend release."""
from pathlib import Path
import sys
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from frontend_release import fetch

def render_page(path):
    model = Path(path).resolve().relative_to(ROOT).parts[0].lower()
    if model not in ('tc1', 'm1', 'a1'):
        raise ValueError('Unknown frontend model: ' + model)
    return (fetch(ROOT / 'frontend.lock.json') / 'devices' / model / 'index.html').read_bytes()

if __name__ == '__main__':
    sys.stdout.buffer.write(render_page(sys.argv[1]))
