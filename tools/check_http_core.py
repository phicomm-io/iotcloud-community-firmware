#!/usr/bin/env python3
"""Check actual linked shared core and conservative application stack call paths.

GCC .su frames plus explicit SDK/JSON/crypto reserves; not runtime watermarks.
"""
from pathlib import Path
import re
import subprocess
import sys

def check(modules, model, nm, elf):
    modules=Path(modules)
    frames={}
    for directory in (modules/model,modules/'phicomm_httpd'):
        for path in directory.rglob('*.su'):
            for line in path.read_text().splitlines():
                location,size,kind=line.split('\t')
                name=location.rsplit(':',1)[-1].split('.')[0]
                if kind=='static':frames[name]=max(frames.get(name,0),int(size))
    budget=int(re.search(r'#define PH_HTTP_STACK_BYTES (\d+)u',(Path(__file__).resolve().parents[1]/'common/httpd/http.h').read_text()).group(1))
    status=model.lower()+'_status_json'
    base=['owner','ph_http_server_step','acquire','action','begin','prepare']
    dispatch=['service_query','service_command','service_product']
    if 'service_product_inner' in frames:dispatch+=['service_product_inner']
    if 'service_command_execute' in frames:dispatch+=['service_command_execute']
    command=['owner','ph_http_server_step','read_body','complete','service_command','service_command_execute','service_product','ph_http_json']
    if 'service_product_inner' in frames:command+=['service_product_inner']
    status_path=base+dispatch+[status]
    if status+'_view' in frames:status_path+=[status+'_view']
    paths={
      'HTTP status':(3072,status_path),
      'HTTP command status':(3072,command+[status]+([status+'_view'] if status+'_view' in frames else [])),
      # Builder frames have returned before encoding. Count the bounded
      # automatic traversal explicitly, outside SDK/native/RTOS reserve.
      'HTTP GET JSON encode':(3072,base+['ph_http_json','json_owned_to_wire','json_write','json_primitive','json_append']),
      'HTTP POST JSON encode':(3072,['owner','ph_http_server_step','read_body','complete','ph_http_json','json_owned_to_wire','json_write','json_primitive','json_append']),
      'HTTP response':(3072,['owner','ph_http_server_step','respond','transmit']),
      'HTTP OTA stream':(4096,['owner','ph_http_server_step','read_body','body_chunk',model.lower()+'_ota_write','signed_ota_write','uECC_verify','EccPoint_mult']),
      'HTTP OTA validation':(4096,['owner','ph_http_server_step','read_body','complete',model.lower()+'_ota_finish','signed_ota_finish']),
    }
    if model=='A1':paths['HTTP command control']=(3072,command+['a1_control_local','feature_group_control','group_send','parse'])
    # GCC inlines send_part into step in the pinned optimized build. A separate
    # frame must be counted if a compiler/optimization change emits it.
    if 'send_part' in frames:paths['HTTP response'][1].append('send_part')
    for label,(reserve,names) in paths.items():
        missing=set(names)-frames.keys()
        if missing:raise ValueError(f'{label}: missing actual .su frames: {sorted(missing)}')
        size=sum(frames[n] for n in names)
        if size+reserve>budget:raise ValueError(f'{label}: {size}+{reserve}>{budget}')
        print(f'{label}: {size} application + {reserve} reserve <= {budget} bytes')
    symbols=subprocess.check_output([nm,'--defined-only',elf],text=True)
    for symbol in ('ph_http_server_step','ph_http_parser_feed','ph_http_start'):
        if len(re.findall(r'\b'+symbol+r'$',symbols,re.M))!=1:raise ValueError('Expected one linked '+symbol)
    forbidden=('httpd_main_thread','httpd_handle_message','httpd_parse_hdr_tags','httpd_parse_request','httpd_wsgi_init','http_accept_worker','http_worker')
    if any(re.search(r'\b'+n+r'$',symbols,re.M) for n in forbidden):raise ValueError('Second HTTP daemon/parser linked')
    archive=modules.parent/'libraries/phicomm_httpd.a'
    if not archive.exists():raise ValueError('Shared core archive missing')
    print(f'{model}: ELF links one common parser/scheduler; no legacy daemon/worker symbols')
if __name__=='__main__':
    try:check(*sys.argv[1:])
    except (ValueError,OSError,subprocess.CalledProcessError) as e:sys.exit(str(e))
