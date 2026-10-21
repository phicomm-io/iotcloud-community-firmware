#!/usr/bin/env python3
"""Reject an ELF whose retained HTTP trace overlaps initialized/BSS/heap RAM."""
from pathlib import Path
import subprocess
import sys
nm,elf=sys.argv[1:]
values={}
for line in subprocess.check_output([nm,'-S','--defined-only',elf],text=True).splitlines():
 fields=line.split()
 if len(fields) in (3,4):values[fields[-1]]=(int(fields[0],16),int(fields[1],16) if len(fields)==4 else 0)
start,size=values['retained']
assert 0<size<=512
assert values['link_global_data_end'][0]<=start
assert start+size<=values['link_bss_location'][0]<=values['link_bss_end'][0]<=values['_ram_end_'][0]
sections=subprocess.check_output([str(Path(nm).with_name('arm-none-eabi-readelf')),'-W','-S',elf],text=True)
assert any('.http_retained' in line and 'NOBITS' in line for line in sections.splitlines())
print(f'TC1 retained HTTP trace {size} B at {start:#x}: NOBITS, outside initialized data/BSS clear/heap reuse')
