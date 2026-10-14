#!/usr/bin/env python3
import gzip,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/"common/web"))
from page import render_page
page=gzip.compress(render_page(Path(__file__).parent/"web/index.html"),compresslevel=9,mtime=0)
text="const unsigned char web_index_html[] = {"+",".join("0x%02x"%b for b in page)+"};\n"
(Path(__file__).parent/"web_data.c").write_text(text)
print("TC1 offline UI: %d bytes gzip"%len(page))
