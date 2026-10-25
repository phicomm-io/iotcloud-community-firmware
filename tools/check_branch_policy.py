#!/usr/bin/env python3
"""Check PR routing and require release tags to belong to main."""
import json
import os
from pathlib import Path
import re
import subprocess


def topic(branch):
    return bool(re.fullmatch(r'(feature|hotfix)/.+', branch))


def check(event_name, event, ref, root=None):
    if event_name == 'pull_request':
        pull = event['pull_request']
        base, head = pull['base']['ref'], pull['head']['ref']
        allowed = ((base == 'dev' and (topic(head) or head == 'main')) or
                   (base == 'main' and (head == 'dev' or (topic(head) and head.startswith('hotfix/')))))
        if not allowed:
            raise ValueError('Use feature/* -> dev -> main; hotfix/* -> main, then sync main -> dev.')
    elif ref.startswith('refs/tags/'):
        tag = ref.removeprefix('refs/tags/')
        if not re.fullmatch(r'v[1-9]\d*\.(0|[1-9]\d*)\.(0|[1-9]\d*)', tag):
            raise ValueError('Only stable v1+ tags may release; archive/0.9.x is frozen.')
        command = ['git'] + (['-C', str(root)] if root else [])
        head = subprocess.check_output(command + ['rev-parse', 'HEAD'], text=True).strip()
        target = subprocess.check_output(command + ['rev-parse', f'refs/tags/{tag}^{{commit}}'], text=True).strip()
        if head != target:
            raise ValueError('Release tag does not point to the checked-out commit')
        subprocess.run(command + ['merge-base', '--is-ancestor', 'HEAD', 'origin/main'], check=True)
    elif event_name in ('push', 'workflow_dispatch'):
        branch = ref.removeprefix('refs/heads/')
        if not ref.startswith('refs/heads/') or (branch not in ('main', 'dev') and not topic(branch)):
            raise ValueError('Build main, dev, feature/* or hotfix/*; archive/0.9.x is frozen.')


if __name__ == '__main__':
    check(os.environ.get('GITHUB_EVENT_NAME', ''),
          json.loads(Path(os.environ['GITHUB_EVENT_PATH']).read_text()),
          os.environ.get('GITHUB_REF', ''))
