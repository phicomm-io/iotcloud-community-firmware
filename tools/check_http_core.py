#!/usr/bin/env python3
"""Check actual linked shared core and conservative application stack call paths.

GCC .su frames plus explicit SDK/JSON/crypto reserves; not runtime watermarks.
"""
from pathlib import Path
import re
import subprocess
import sys

def read_json_allocator_frames(modules,frames):
    # Only json_heap.su is imported from the SDK JSON module. Static allocator
    # helpers must not collide with unrelated permit/created symbols. Ignore its
    # weak hook stubs: actual strong application hooks above are authoritative.
    allocator_files=list(Path(modules).rglob('json_heap.su'))
    if len(allocator_files)!=1:raise ValueError('Expected one actual SDK json_heap.su')
    for line in allocator_files[0].read_text().splitlines():
        location,size,kind=line.split('\t')
        name=location.rsplit(':',1)[-1].split('.')[0]
        if kind!='static' or name in ('json_heap_lock','json_heap_unlock','json_heap_permit','json_heap_failure'):continue
        if name in ('permit','created','increment'):name='json_heap.'+name
        frames[name]=max(frames.get(name,0),int(size))
    # Owned MQTT receipt cleanup calls the actual SDK printbuf destructor.
    # Import only that leaf, keeping unrelated SDK helpers out of the namespace.
    destructor_files=list(Path(modules).rglob('printbuf.su'))
    if len(destructor_files)>1:raise ValueError('Expected one actual SDK printbuf.su')
    for path in destructor_files:
        for line in path.read_text().splitlines():
            location,size,kind=line.split('\t')
            if kind=='static' and location.rsplit(':',1)[-1].split('.')[0]=='printbuf_free':
                frames['printbuf_free']=max(frames.get('printbuf_free',0),int(size))

def check_tc1_ota_numeric(frames, budget):
    try:
        from .json_numeric_stack import numeric_tails
    except ImportError:
        from json_numeric_stack import numeric_tails
    # Streaming download retains ota_fetch's HTTP header/chunk scratch while
    # the progress callback formats. Network/TLS work is a sibling of this
    # callback; retain the unchanged SDK/RTOS allowance, not both branches.
    prefix = ['download_worker', 'ota_fetch', 'fetch_write', 'OtaServerStatusHandler']
    for branch, tail in numeric_tails(frames, 'fixed').items():
        names = prefix + tail
        missing = set(names) - frames.keys()
        if missing:
            raise ValueError('OTA progress numeric: missing .su frames '+str(sorted(missing)))
        size = sum(frames[name] for name in names)
        if size + 3072 > budget:
            raise ValueError(f'OTA progress numeric {branch}: {size}+3072>{budget}')
        print(f'OTA progress numeric {branch}: {size} application + 3072 reserve <= {budget} bytes')


def check(modules, model, nm, elf):
    modules=Path(modules)
    frames={}
    verify_frames={}
    for directory in (modules/model,modules/'phicomm_httpd',modules/'phicomm_mqtt'):
        for path in directory.rglob('*.su'):
            for line in path.read_text().splitlines():
                location,size,kind=line.split('\t')
                emitted=location.rsplit(':',1)[-1]
                name=emitted.split('.')[0]
                if kind=='static':
                    frames[name]=max(frames.get(name,0),int(size))
                    if name=='tc1_ota_verify':verify_frames[emitted]=max(verify_frames.get(emitted,0),int(size))
    read_json_allocator_frames(modules,frames)
    try:
        from .json_numeric_stack import read_numeric_frames, add_numeric_paths
    except ImportError:
        from json_numeric_stack import read_numeric_frames, add_numeric_paths
    read_numeric_frames(modules,frames)
    # The pinned compiler splits verification into an 8-byte entry and a
    # 632-byte .part body; both can be live, unlike alternative clones.
    if verify_frames:frames['tc1_ota_verify']=sum(verify_frames.values())
    budget=int(re.search(r'#define PH_HTTP_STACK_BYTES (\d+)u',(Path(__file__).resolve().parents[1]/'common/httpd/http.h').read_text()).group(1))
    status=model.lower()+'_status_json'
    base=['owner','ph_http_server_step','acquire','action','begin','prepare']
    dispatch=['ph_http_query','service_query_wire','service_query','service_command','service_product']
    if 'service_product_inner' in frames and model!='TC1':dispatch+=['service_product_inner']
    if 'service_command_execute' in frames:dispatch+=['service_command_execute']
    command=['owner','ph_http_server_step','read_body','complete','ph_http_command','service_command_wire','service_command','service_command_execute','service_product','ph_http_json']
    if 'service_product_inner' in frames:command+=['service_product_inner']
    status_path=base+dispatch+[status]
    if status+'_view' in frames:status_path+=[status+'_view']
    paths={
      'HTTP status':(3072,status_path),
      'HTTP JSON admission':(3072,base+['ph_http_claim','service_json_claim']),
      'HTTP GET JSON scope':(3072,base+['ph_http_claim']+[n for n in ('graph_begin',) if n in frames]+['service_json_scope_begin','json_heap_lock']),
      'HTTP POST JSON scope':(3072,['owner','ph_http_server_step','read_body','complete','ph_http_body_json']+[n for n in ('graph_begin',) if n in frames]+['service_json_scope_begin','json_heap_lock']),
      'HTTP GET scope owner':(3072,base+['ph_http_claim']+[n for n in ('graph_begin',) if n in frames]+['ph_http_json_owner']),
      'HTTP POST scope owner':(3072,['owner','ph_http_server_step','read_body','complete','ph_http_body_json']+[n for n in ('graph_begin',) if n in frames]+['ph_http_json_owner']),
      'HTTP command failure attribution':(3072,['owner','ph_http_server_step','read_body','complete','service_command','service_caller_graph_failed','json_heap_lock']),
      'HTTP query failure attribution':(3072,base+['service_query','service_command','service_caller_graph_failed','json_heap_lock']),
      'HTTP command failure check':(3072,['owner','ph_http_server_step','read_body','complete','ph_http_graph_failed','service_json_scope_failed','json_heap_lock']),
      'HTTP response failure check':(3072,base+['ph_http_json','ph_http_graph_failed','service_json_scope_failed','json_heap_lock']),
      'HTTP response scope cleanup':(3072,base+['ph_http_json']+[n for n in ('graph_end',) if n in frames]+['service_json_scope_end','json_heap_lock']),
      'HTTP request scope cleanup':(3072,['owner','ph_http_server_step']+[n for n in ('close_slot',) if n in frames]+['release','ph_http_product_release']+[n for n in ('graph_end',) if n in frames]+['service_json_scope_end','json_heap_lock']),
      'HTTP lease cleanup':(3072,['owner','ph_http_server_step']+[n for n in ('close_slot',) if n in frames]+['release','ph_http_product_release','service_json_release','json_heap_lock']),
      'HTTP command status':(3072,command+[status]+([status+'_view'] if status+'_view' in frames else [])),
      # Builder frames have returned before encoding. Count the bounded
      # automatic traversal explicitly, outside SDK/native/RTOS reserve.
      'HTTP GET JSON encode':(3072,base+['ph_http_query','service_query_wire','json_owned_to_wire','json_owned_to_wire_result','json_write','json_primitive','json_append']),
      'HTTP POST JSON encode':(3072,['owner','ph_http_server_step','read_body','complete','ph_http_command','service_command_wire','json_owned_to_wire','json_owned_to_wire_result','json_write','json_primitive','json_append']),
      'HTTP MQTT diagnostics':(3072,base+(['memory_response'] if 'memory_response' in frames else [])+['ph_mqtt_diag_append','ph_mqtt_snapshot','ph_mqtt_core_snapshot']),
      'HTTP response':(3072,['owner','ph_http_server_step','respond','transmit']),
      'HTTP OTA stream':(4096,['owner','ph_http_server_step','read_body','body_chunk',model.lower()+'_ota_write','signed_ota_write','uECC_verify','EccPoint_mult']),
      'HTTP OTA validation':(4096,['owner','ph_http_server_step','read_body','complete',('tc1_ota_verify' if model=='TC1' else model.lower()+'_ota_finish'),'signed_ota_finish']),
    }
    if model=='TC1':
        paths['HTTP OTA MD5 validation']=(4096,['owner','ph_http_server_step','read_body','complete','tc1_ota_verify','md5_range'])
        paths['HTTP OTA activation']=(4096,['owner','ph_http_server_step','close_slot','release','tc1_ota_activate']+(['send_part'] if 'send_part' in frames else []))
    if model!='TC1':paths['HTTP OTA recovery']=(4096,['owner','ph_http_server_step','admission','ota_job_poll','feature_resume_step'])
    if model=='A1':
        control=['a1_control_json','a1_control_apply']+[n for n in ('control_broadcast',) if n in frames]
        paths['HTTP command control']=(3072,command+control+['feature_group_control','group_send','parse'])
        paths['HTTP control persistence']=(3072,command+control+['feature_fan_apply','install_config','feature_config_save'])
    if model in ('A1','TC1'):
        for helper in ('bemfa_config_encode','bemfa_config_values','a1_mqtt_config_save'):
            if helper in frames:
                paths['HTTP config '+helper]=(3072,(base+dispatch if helper.endswith('encode') else command)+[helper])
    if model in ('A1','TC1'):
        business=['owner','bemfa_dispatch','service_command','service_command_execute','service_product','service_product_inner']
        paths['HTTP Bemfa command']=(4096,business)
        paths['HTTP Bemfa config fence']=(4096,['owner','bemfa_dispatch','bemfa_config_get'])
        if model=='A1':paths['HTTP Bemfa control']=(4096,business+control+['feature_group_control','group_send','parse'])
    # GCC inlines send_part into step in the pinned optimized build. A separate
    # frame must be counted if a compiler/optimization change emits it.
    if 'send_part' in frames:paths['HTTP response'][1].append('send_part')
    # Model wrappers keep the full scratch out of the live stack frame. State
    # capture returns before pure encoding, so audit the two reachable tails
    # separately rather than adding their sequential frames together.
    try:
        from .status_stack import status_paths as direct_status_paths
    except ImportError:
        from status_stack import status_paths as direct_status_paths
    expanded={}
    for label,(reserve,names) in paths.items():
        if status not in names:
            expanded[label]=(reserve,names);continue
        common=[n for n in names if not (model=='TC1' and n=='service_product_inner')]
        for n in (status+'_view',model.lower()+'_status_full'):
            if n in frames and n not in common:common.append(n)
        for key,(stack,spare) in direct_status_paths(frames,model,label,common,reserve,compat=True).items():expanded[key]=(spare,stack)
    paths=expanded
    for label,prefix in [('HTTP direct GET',base+['ph_http_query','service_query_wire','service_product_status_wire']),
                         ('HTTP direct POST',['owner','ph_http_server_step','read_body','complete','ph_http_command','service_command_wire','service_product_status_wire'])]:
        for key,(stack,spare) in direct_status_paths(frames,model,label,prefix,3072).items():paths[key]=(spare,stack)
    for label,prefix in [('GET',base+['ph_http_query']),('POST',['owner','ph_http_server_step','read_body','complete','ph_http_command'])]:
        paths['HTTP direct '+label+' scope cleanup']=(3072,prefix+['ph_http_wire_take']+[n for n in ('graph_end',) if n in frames]+['service_json_scope_end','json_heap_lock'])
    # Allocator failure observation can also run on HTTP's thread while an
    # MQTT optional scope exists. Count the strong hook and actual allocator
    # frames, retaining the caller's graph/encoder path; no new RTOS allowance.
    for label,(reserve,names) in list(paths.items()):
        if 'json_write' not in names and 'device_status_encode' not in names:continue
        for api in ('json_heap_malloc','json_heap_calloc','json_heap_resize','json_heap_strdup'):
            allocator=[api]
            for tail in ('permit','created'):
                paths[label+' allocator '+api+' '+tail]=(reserve,names+allocator+['json_heap.'+tail,'json_heap_failure'])
    # The primitive callback formats before json_append; they are sibling tails.
    for label,(reserve,names) in list(paths.items()):
        if 'json_primitive' in names and 'allocator' not in label:
            prefix=names[:names.index('json_primitive')+1]+['json_object_double_to_json_string']
            add_numeric_paths(paths,frames,prefix,reserve,label+' numeric', 'format')
        if 'parse' in names:
            prefix=names[:names.index('parse')+1]+['json_tokener_parse_ex']
            add_numeric_paths(paths,frames,prefix,reserve,label+' numeric', 'scan')
    add_numeric_paths(paths,frames,
        ['owner','ph_http_server_step','read_body','complete','ph_http_body_json','service_parse','json_tokener_parse_ex'],
        3072,'HTTP POST numeric parse','scan')
    from check_mqtt_core import context_frames
    context_frames(names for reserve,names in paths.values())
    for label,(reserve,names) in paths.items():
        missing=set(names)-frames.keys()
        if missing:raise ValueError(f'{label}: missing actual .su frames: {sorted(missing)}')
        size=sum(frames[n] for n in names)
        if size+reserve>budget:raise ValueError(f'{label}: {size}+{reserve}>{budget}')
        print(f'{label}: {size} application + {reserve} reserve <= {budget} bytes')
    from check_mqtt_core import check as check_mqtt
    check_mqtt(frames,model)
    if model=='TC1':
        source=(Path(__file__).resolve().parents[1]/'TC1/ota_server/ota_server.c').read_text()
        ota_budget=int(re.search(r'download_worker,(\d+),',source).group(1))
        for branch,tail in [('signature',['signed_ota_finish']),('MD5',['md5_range']),('activation',['tc1_ota_activate'])]:
            names=['download_worker','tc1_ota_finish']+(['tc1_ota_verify'] if branch!='activation' else [])+tail
            missing=set(names)-frames.keys()
            if missing:raise ValueError(f'OTA download {branch}: missing .su frames {sorted(missing)}')
            size=sum(frames[n] for n in names)
            if size+3072>ota_budget:raise ValueError(f'OTA download {branch}: {size}+3072>{ota_budget}')
            print(f'OTA download {branch}: {size} application + 3072 reserve <= {ota_budget} bytes')
        check_tc1_ota_numeric(frames,ota_budget)
    symbols=subprocess.check_output([nm,'--defined-only',elf],text=True)
    for symbol in ('ph_http_server_step','ph_http_parser_feed','ph_http_start','ph_mqtt_session_run','ph_mqtt_poll','ph_mqtt_enqueue_generation'):
        if len(re.findall(r'\b'+symbol+r'$',symbols,re.M))!=1:raise ValueError('Expected one linked '+symbol)
    forbidden=('httpd_main_thread','httpd_handle_message','httpd_parse_hdr_tags','httpd_parse_request','httpd_wsgi_init','http_accept_worker','http_worker')
    if any(re.search(r'\b'+n+r'$',symbols,re.M) for n in forbidden):raise ValueError('Second HTTP daemon/parser linked')
    archive=modules.parent/'libraries/phicomm_httpd.a'
    if not archive.exists():raise ValueError('Shared core archive missing')
    if not (modules.parent/'libraries/phicomm_mqtt.a').exists():raise ValueError('Shared MQTT archive missing')
    print(f'{model}: ELF links one common parser/scheduler; no legacy daemon/worker symbols')
if __name__=='__main__':
    try:check(*sys.argv[1:])
    except (ValueError,OSError,subprocess.CalledProcessError) as e:sys.exit(str(e))
