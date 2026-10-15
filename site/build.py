#!/usr/bin/env python3
"""Build the public allowlist; never copy the private source tree or Wiki."""
import argparse,html,shutil,sys
import markdown
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser();p.add_argument('output',type=Path);a=p.parse_args();out=a.output;out.mkdir(parents=True,exist_ok=True)
for name in ['index.html','style.css','releases.js']:shutil.copyfile(ROOT/'site'/name,out/name)
(out/'.nojekyll').touch()
for name in ['使用说明','刷机','资源来源']:
 source=ROOT/'docs/public'/f'{name}.md'
 if source.exists():
  dest=out/'guides';dest.mkdir(exist_ok=True)
  (dest/f'{name}.html').write_text('<!doctype html><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><link rel="stylesheet" href="../style.css"><main><a href="../">← 首页</a><article>'+markdown.markdown(source.read_text(),extensions=['tables','fenced_code'])+'</article></main>')
# Demos are rendered from the same firmware sources with an isolated adapter.
sys.path.insert(0,str(ROOT/'common/web'))
from page import render_page
for model,source in [('tc1','TC1/http_server/web/index.html'),('m1','M1/web/index.html'),('a1','A1/web/index.html')]:
 dest=out/'demos'/model;dest.mkdir(parents=True,exist_ok=True)
 adapter=(ROOT/'site/demo.js').read_text().replace('__MODEL__',model)
 text=render_page(ROOT/source).decode();text=text.replace('<head>','<head><script>'+adapter+'</script>',1)
 (dest/'index.html').write_text(text)

shutil.copytree(ROOT/'docs/public/assets',out/'guides/assets',dirs_exist_ok=True)
