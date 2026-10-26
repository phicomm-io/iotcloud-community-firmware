#!/usr/bin/env python3
"""Freeze private Wiki Release-Log.md before tagging; CI reads only this snapshot."""
import argparse, hashlib, json, re, subprocess
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
from release_version import TAG_PATTERN as TAG, parse_tag

def extract(text, tag):
    parse_tag(tag)
    sections = [section for section in re.finditer(r'^##[ \t]+(v[^\s]+)[ \t]*$', text, re.M)
                if TAG.fullmatch(section[1])]
    matches = [i for i,s in enumerate(sections) if s[1] == tag]
    if len(matches) != 1: raise ValueError(f'{tag}: expected exactly one release section')
    i = matches[0]
    body = text[sections[i].end():sections[i+1].start() if i+1<len(sections) else len(text)].strip()
    if len(body)<20 or re.search(r'(?i)\b(TODO|TBD|placeholder)\b|待补充|待填写',body):
        raise ValueError(f'{tag}: release notes are empty or unfinished')
    return body+'\n'

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('tag');p.add_argument('--wiki',type=Path);p.add_argument('--output',type=Path)
    a=p.parse_args();dest=ROOT/'docs/release'
    if a.wiki:
        tags=subprocess.check_output(['git','-C',str(ROOT),'tag','--merged','HEAD','--list','v*'],text=True).splitlines()
        versions=[parse_tag(t).key for t in tags if TAG.fullmatch(t)]
        if not TAG.fullmatch(a.tag) or (versions and parse_tag(a.tag).key<=max(versions)):
            p.error('new release must increment the latest existing SemVer tag')
        if subprocess.check_output(['git','-C',str(a.wiki),'status','--porcelain'],text=True).strip():
            p.error('commit Wiki changes before freezing the snapshot')
        text=(a.wiki/'Release-Log.md').read_text();extract(text,a.tag)
        dest.mkdir(parents=True,exist_ok=True);(dest/'Release-Log.md').write_text(text)
        (dest/'wiki-snapshot.json').write_text(json.dumps({'wiki_commit':subprocess.check_output(['git','-C',str(a.wiki),'rev-parse','HEAD'],text=True).strip(),'sha256':hashlib.sha256(text.encode()).hexdigest()},indent=2)+'\n')
    text=(dest/'Release-Log.md').read_text();meta=json.loads((dest/'wiki-snapshot.json').read_text())
    if hashlib.sha256(text.encode()).hexdigest()!=meta['sha256']:p.error('Wiki snapshot hash mismatch')
    body=extract(text,a.tag)
    if a.output:a.output.write_text(body)
    else: print(body,end='')
if __name__=='__main__':main()
