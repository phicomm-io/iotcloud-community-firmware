#!/usr/bin/env python3
"""Import already-published GitHub releases without rebuilding or re-signing."""
import json,subprocess
from pathlib import Path
from catalog import make
ROOT=Path(__file__).resolve().parents[2]
releases=json.loads(subprocess.check_output(['gh','api','repos/FUjr/phicomm-community-firmware/releases'],text=True))
for r in reversed(releases):
 tag=r['tag_name']
 if not tag.startswith('v') or r['draft'] or r['prerelease']:continue
 dest=ROOT/'build/history'/tag;dest.mkdir(parents=True,exist_ok=True)
 subprocess.run(['gh','release','download',tag,'-R','FUjr/phicomm-community-firmware','--dir',str(dest),'--clobber'],check=True)
 subprocess.run(['sha256sum','-c','SHA256SUMS'],cwd=dest,check=True)
 commit=subprocess.check_output(['git','rev-list','-n','1',tag],cwd=ROOT,text=True).strip()
 notes=subprocess.check_output(['python3','tools/release/notes.py',tag],cwd=ROOT,text=True)
 make(tag,commit,dest,notes,legacy=True)
 subprocess.run(['python3','tools/release/publish.py',str(dest),'--archive-only'],cwd=ROOT,check=True)
