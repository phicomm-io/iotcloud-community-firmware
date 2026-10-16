#!/usr/bin/env python3
"""Import already-published GitHub releases without rebuilding or re-signing."""
import argparse,json,subprocess
from pathlib import Path
from catalog import make
ROOT=Path(__file__).resolve().parents[2]
parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--from-tag',default='v0.0.0');args=parser.parse_args()
releases=json.loads(subprocess.check_output(['gh','api','repos/phicomm-io/phicomm-community-firmware/releases'],text=True))
for r in reversed(releases):
 tag=r['tag_name']
 if not tag.startswith('v') or r['draft'] or r['prerelease']:continue
 if tuple(map(int,tag[1:].split('.')))<tuple(map(int,args.from_tag[1:].split('.'))):continue
 dest=ROOT/'build/history'/tag;dest.mkdir(parents=True,exist_ok=True)
 subprocess.run(['gh','release','download',tag,'-R','phicomm-io/phicomm-community-firmware','--dir',str(dest),'--clobber'],check=True)
 subprocess.run(['sha256sum','-c','SHA256SUMS'],cwd=dest,check=True)
 commit=subprocess.check_output(['git','rev-list','-n','1',tag],cwd=ROOT,text=True).strip()
 notes=subprocess.check_output(['python3','tools/release/notes.py',tag],cwd=ROOT,text=True)
 make(tag,commit,dest,notes,legacy=True)
 subprocess.run(['python3','tools/release/publish.py',str(dest),'--archive-only'],cwd=ROOT,check=True)
