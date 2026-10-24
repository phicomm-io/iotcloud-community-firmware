#!/usr/bin/env python3
"""Add website navigation to copied demos, including retained platform builds."""
import argparse
import re
import shutil
from pathlib import Path

HERE = Path(__file__).resolve().parent
ASSETS = HERE if (HERE / 'demo-nav.js').is_file() else HERE.parent
HOME_LINK = ('<a class="demo-site-home" data-demo-home href="../../">'
             '<span aria-hidden="true">←</span> '
             '<span data-demo-home-label>返回网站首页</span></a>')


def add_navigation(output):
    output = Path(output)
    for name in ('demo-nav.css', 'demo-nav.js'):
        if (ASSETS / name).resolve() != (output / name).resolve():
            shutil.copyfile(ASSETS / name, output / name)
    for page in sorted((output / 'demos').glob('*/index.html')):
        text = page.read_text(encoding='utf-8')
        if 'data-demo-home' in text:
            continue
        text, head = re.subn(r'</head>', '<link rel="stylesheet" href="../../demo-nav.css">'
                            '<script src="../../demo-nav.js" defer></script></head>', text, count=1, flags=re.I)
        text, body = re.subn(r'(<body\b[^>]*>)', lambda match: match[0] + HOME_LINK,
                            text, count=1, flags=re.I)
        if head != 1 or body != 1:
            raise ValueError(f'Demo has no HTML head/body: {page}')
        page.write_text(text, encoding='utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    add_navigation(parser.parse_args().output)
