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


# Keep the SDK JSON allocator / OTA numeric helpers above available to the
# independent HA and OTA gates. Native HTTP command/status paths use neither.
HTTP_BUDGET=12288
HTTP_RESERVE=3072


def linked_symbols(output):
    result={}
    for line in output.splitlines():
        fields=line.split()
        if len(fields)<2:continue
        name=fields[-1].split('.')[0]
        result[name]=result.get(name,0)+1
    return result


def check_symbols(symbols):
    required=('ph_http_server_step','ph_http_parser_feed','ph_http_start',
              'ph_mqtt_session_run','ph_mqtt_poll','ph_mqtt_enqueue_generation',
              'service_command_mp','service_query_mp','service_product_mp',
              'service_product_status_mp','device_status_mp','phv_parse',
              'phw_cached_value','phw_number','ph_buf_reserve')
    for name in required:
        if symbols.get(name)!=1:raise ValueError('Expected one linked '+name)
    forbidden=('httpd_main_thread','httpd_handle_message','httpd_parse_hdr_tags',
               'httpd_parse_request','httpd_wsgi_init','http_accept_worker','http_worker',
               'service_command_wire','service_query_wire','service_command',
               'service_query','service_product_status_wire','ph_binary_parse',
               'json_owned_to_wire','json_owned_to_wire_result','device_status_encode',
               'device_status_wire','a1_status_json','m1_status_json','tc1_status_json')
    for name in forbidden:
        if name in symbols:raise ValueError('Legacy HTTP/JSON product bridge linked: '+name)


class StackFrames(dict):
    def __init__(self):
        super().__init__();self.unbounded={}


def read_frames(modules,model):
    frames=StackFrames();verify={}
    for directory in (Path(modules)/model,Path(modules)/'phicomm_httpd',Path(modules)/'phicomm_mqtt'):
        for path in directory.rglob('*.su'):
            for line in path.read_text().splitlines():
                location,size,kind=line.split('\t');full=location.rsplit(':',1)[-1];name=full.split('.')[0]
                if kind!='static':frames.unbounded[name]=kind
                frames[name]=max(frames.get(name,0),int(size))
                if name=='tc1_ota_verify':verify[full]=max(verify.get(full,0),int(size))
    # GCC splits this entry into an entry and retained .part body, unlike
    # alternative .isra clones. Preserve the previous conservative sum.
    if verify:frames['tc1_ota_verify']=sum(verify.values())
    return frames


def http_paths(frames,model,symbols=None):
    try:
        from .status_stack import emitted, status_paths, writer_paths
    except ImportError:
        from status_stack import emitted, status_paths, writer_paths
    if model not in ('A1','M1','TC1'):raise ValueError('Unknown HTTP model: '+model)
    extra=lambda names:emitted(frames,names,symbols)
    owner=['owner','ph_http_server_step']
    get=owner+extra(['acquire','action'])+['begin','prepare','ph_http_query','service_query_mp','service_command_mp']
    post=owner+extra(['read_body'])+['complete','ph_http_command_mp','service_command_mp']
    paths={}
    for label,prefix in [('GET',get),('POST',post)]:
        # Decode returns before business dispatch. Iterator scans are iterative,
        # not recursive: charge its complete bounded local traversal once.
        decode=prefix+extra(['mp_decode'])
        for api in ['phv_parse','phv_keys','phv_get']:
            paths['HTTP '+label+' decode '+api]=(HTTP_RESERVE,decode+[api]+extra(['phv_iter','phv_child_reader','phv_next','phv_scan','phv_duplicate','phv_skip_validated','phv_head','binary_token','binary_take','ph_wire_utf8']))
        dispatch=prefix+['mp_dispatch']
        status=dispatch+['mp_cached','service_product_status_mp']
        for key,(names,reserve) in status_paths(frames,model,'HTTP '+label+' full status',status,HTTP_RESERVE,symbols=symbols).items():paths[key]=(reserve,names)
        cached=dispatch+['mp_envelope','phw_cached_value']+extra(['phw_prefix','phw_quote','phw_head','phw_bytes'])
        paths['HTTP '+label+' cached status copy']=(HTTP_RESERVE,cached+['ph_buf_reserve','ph_wire_malloc'])
        paths['HTTP '+label+' receipt copy']=(HTTP_RESERVE,dispatch+extra(['mp_copy'])+['ph_buf_new','ph_wire_malloc'])
        for key,(names,reserve) in writer_paths(frames,'HTTP '+label+' envelope',dispatch+['mp_envelope'],HTTP_RESERVE,symbols).items():paths[key]=(reserve,names)
        for helper in ('service_product_mp','service_product_capabilities_mp','service_product_info_mp'):
            for key,(names,reserve) in writer_paths(frames,'HTTP '+label+' '+helper,dispatch+[helper],HTTP_RESERVE,symbols).items():paths[key]=(reserve,names)
        # Typed product helpers retain configuration/scan scratch while walking
        # nested parameter values. Their validation tails are distinct from the
        # top-level envelope parser, and must not disappear from the bound.
        business=dispatch+['service_product_mp']+extra(['smp_config','smp_control',
                  'smp_music','smp_text','smp_int','smp_has'])
        paths['HTTP '+label+' business parameter validation']=(HTTP_RESERVE,
            business+['phv_get']+extra(['phv_iter','phv_child_reader','phv_next',
            'phv_scan','phv_duplicate','phv_skip_validated','phv_head',
            'binary_token','binary_take','ph_wire_utf8']))
        paths['HTTP '+label+' logs']=(HTTP_RESERVE,dispatch+extra(['mp_logs'])+['phw_text']+extra(['phw_text_n','phw_prefix','phw_quote','phw_bytes'])+['ph_buf_reserve','ph_wire_malloc'])
    admission=owner+extra(['acquire','action'])+['begin','prepare','ph_http_claim','service_memory_claim']
    paths['HTTP admission']=(HTTP_RESERVE,admission)
    paths['HTTP admission cache reclaim']=(HTTP_RESERVE,admission+extra(['mp_cache_evict'])+['ph_buf_free','ph_wire_free'])
    paths['HTTP cleanup']=(HTTP_RESERVE,owner+extra(['close_slot'])+['release','ph_http_product_release','service_memory_release'])
    paths['HTTP buffer cleanup']=(HTTP_RESERVE,owner+extra(['close_slot'])+['release','ph_http_product_release','ph_buf_free','ph_wire_free'])
    paths['HTTP response']=(HTTP_RESERVE,owner+extra(['respond','response_header','transmit','send_part']))
    paths['HTTP fixed message']=(HTTP_RESERVE,owner+['begin','ph_http_message','phw_text']+extra(['phw_text_n','phw_prefix','phw_quote','phw_bytes']))
    paths['HTTP diagnostics']=(HTTP_RESERVE,owner+['begin']+extra(['memory_response'])+['ph_mqtt_diag_write_mp','ph_mqtt_snapshot','ph_mqtt_core_snapshot'])
    paths['HTTP OTA stream']=(4096,owner+extra(['read_body'])+['body_chunk',model.lower()+'_ota_write','signed_ota_write','uECC_verify','EccPoint_mult']+extra(['uECC_vli_modMult_fast','uECC_vli_modMult','uECC_vli_mmod','uECC_vli_mult']))
    paths['HTTP OTA validation']=(4096,owner+extra(['read_body'])+['complete',('tc1_ota_verify' if model=='TC1' else model.lower()+'_ota_finish'),'signed_ota_finish'])
    if model=='M1':paths['HTTP MCU exchange']=(HTTP_RESERVE,owner+extra(['read_body'])+['complete','m1_mcu_exchange'])
    if model=='TC1':
        paths['HTTP OTA MD5 validation']=(4096,owner+extra(['read_body'])+['complete','tc1_ota_verify','md5_range'])
        paths['HTTP OTA activation']=(4096,owner+extra(['close_slot'])+['release','tc1_ota_activate']+extra(['send_part']))
    else:paths['HTTP OTA recovery']=(4096,owner+['admission','ota_job_poll','feature_resume_step'])
    if model=='A1':
        control=post+['mp_dispatch','service_product_mp']+extra(['smp_control'])+['a1_control_apply']+extra(['control_broadcast','panel_state','a1_control_light_percent'])
        paths['HTTP control persistence']=(HTTP_RESERVE,control+['feature_fan_apply']+extra(['install_config'])+['feature_config_save'])
        paths['HTTP light effect persistence']=(HTTP_RESERVE,post+['mp_dispatch','service_product_mp','feature_mp','feature_light_select_mp']+extra(['install_config'])+['feature_config_save'])
    return paths


def validate_paths(paths,frames,budget=HTTP_BUDGET):
    if budget!=HTTP_BUDGET:raise ValueError('HTTP stack budget must remain 12288 bytes')
    for label,(reserve,names) in paths.items():
        if reserve<HTTP_RESERVE:raise ValueError(label+': insufficient SDK/crypto reserve')
        unbounded=set(names)&getattr(frames,'unbounded',{}).keys()
        if unbounded:raise ValueError(f'{label}: unbounded/nonstatic actual .su frames: {sorted(unbounded)}')
        missing=set(names)-frames.keys()
        if missing:raise ValueError(f'{label}: missing actual .su frames: {sorted(missing)}')
        size=sum(frames[name] for name in names)
        if size+reserve>budget:raise ValueError(f'{label}: {size}+{reserve}>{budget}')
        print(f'{label}: {size} application + {reserve} reserve <= {budget} bytes')


def check_tc1_ota_decimal(frames,budget,symbols=None):
    try:
        from .status_stack import emitted
    except ImportError:
        from status_stack import emitted
    prefix=['download_worker','ota_fetch','fetch_write','OtaServerStatusHandler',
            'ph_decimal_format_float32','pd_format']
    # The big-integer helpers are sibling tails; summing emitted ones is a
    # conservative allocation-free closure. Inline helpers reside in pd_format.
    names=prefix+emitted(frames,['pd_set','pd_shift','pd_word','pd_mul',
                                 'pd_div10','pd_trim'],symbols)
    missing=set(names)-frames.keys()
    if missing:raise ValueError('OTA progress decimal: missing .su frames '+str(sorted(missing)))
    size=sum(frames[n] for n in names)
    if size+3072>budget:raise ValueError(f'OTA progress decimal: {size}+3072>{budget}')
    print(f'OTA progress decimal: {size} application + 3072 reserve <= {budget} bytes')


def check_tc1_ota_progress(frames,budget,symbols=None,decimal=False):
    if decimal:return check_tc1_ota_decimal(frames,budget,symbols)
    # Integer-only percentage logging retains WebLog, then SDK printf/RTOS
    # within the unchanged reserve. The public float state does no formatting.
    names=['download_worker','ota_fetch','fetch_write','OtaServerStatusHandler','WebLog']
    missing=set(names)-frames.keys()
    if missing:raise ValueError('OTA progress integer: missing .su frames '+str(sorted(missing)))
    size=sum(frames[n] for n in names)
    if size+3072>budget:raise ValueError(f'OTA progress integer: {size}+3072>{budget}')
    print(f'OTA progress integer: {size} application + 3072 reserve <= {budget} bytes')


def check(modules,model,nm,elf):
    modules=Path(modules)
    symbols=linked_symbols(subprocess.check_output([nm,'--defined-only',elf],text=True))
    check_symbols(symbols)
    frames=read_frames(modules,model)
    root=Path(__file__).resolve().parents[1]
    budget=int(re.search(r'#define PH_HTTP_STACK_BYTES (\d+)u',(root/'common/httpd/http.h').read_text()).group(1))
    paths=http_paths(frames,model,symbols)
    from check_mqtt_core import context_frames, check as check_mqtt
    context_frames(names for reserve,names in paths.values())
    validate_paths(paths,frames,budget)
    check_mqtt(frames,model)
    if model=='TC1':
        source=(root/'TC1/ota_server/ota_server.c').read_text()
        ota_budget=int(re.search(r'download_worker,(\d+),',source).group(1))
        for branch,tail in [('signature',['signed_ota_finish']),('MD5',['md5_range']),('activation',['tc1_ota_activate'])]:
            names=['download_worker','tc1_ota_finish']+(['tc1_ota_verify'] if branch!='activation' else [])+tail
            missing=set(names)-frames.keys()
            if missing:raise ValueError(f'OTA download {branch}: missing .su frames {sorted(missing)}')
            size=sum(frames[n] for n in names)
            if size+3072>ota_budget:raise ValueError(f'OTA download {branch}: {size}+3072>{ota_budget}')
            print(f'OTA download {branch}: {size} application + 3072 reserve <= {ota_budget} bytes')
        callback=(root/'TC1/ota_server/user_ota.c').read_text()
        check_tc1_ota_progress(frames,ota_budget,symbols,
                              decimal='ph_decimal_format_float32(' in callback)
    for name in ('phicomm_httpd','phicomm_mqtt'):
        if not (modules.parent/'libraries'/ (name+'.a')).exists():raise ValueError('Shared core archive missing: '+name)
    print(f'{model}: linked MessagePack dispatcher/direct status; no JSON product bridge')

if __name__=='__main__':
    try:check(*sys.argv[1:])
    except (ValueError,OSError,subprocess.CalledProcessError) as error:sys.exit(str(error))
