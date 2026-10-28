#!/usr/bin/env python3
"""Actual shared-owner application frames; SDK/Paho/RTOS remain in reserve.

Protocol callbacks copy only. No business path is nested below a PUBACK wait.
These are static upper bounds, never runtime stack watermarks.
"""
from pathlib import Path
import re

def context_frames(paths):
    """Count the explicit context implementation below each primary wrapper.

    Context entry points are noinline. Require emitted frames rather than
    silently treating a missing .su record as an inlined or zero-size call.
    """
    source=(Path(__file__).resolve().parents[1]/'common/mqtt/client.c').read_text()
    entry=set(re.findall(r'noinline\)\)\s+\w+\s+(ctx_ph_mqtt_\w+)\(',source))
    for names in paths:
        expanded=[]
        for name in names:
            expanded.append(name)
            internal='ctx_'+name if name.startswith('ph_mqtt_') else 'ctx_ph_mqtt_'+name[3:] if name.startswith('bm_') else ''
            if internal in entry:expanded.append(internal)
        # Owner checks can also be reached through internal guard callbacks.
        if 'current' in expanded and 'ctx_ph_mqtt_owner' not in expanded:
            expanded.append('ctx_ph_mqtt_owner')
        names[:]=expanded

def check(frames,model):
    """Native MP only. Explicit public frames; bounded HA recursion is separate."""
    try:
        from .status_stack import emitted, status_paths, diagnostic_paths, writer_paths
    except ImportError:
        from status_stack import emitted, status_paths, diagnostic_paths, writer_paths
    source=(Path(__file__).resolve().parents[1]/('TC1/mqtt_server/user_mqtt_client.c' if model=='TC1' else model+'/mqtt.c')).read_text()
    budget=int(re.search(r'mqtt_thread_stack_size = (0x[0-9a-fA-F]+|[0-9]+)',source).group(1),0) if model=='TC1' else int(re.search(r'mqtt_worker,\s*(\d+),',source).group(1))
    reserve=3072 if model=='TC1' else 4096
    worker='MqttClientThread' if model=='TC1' else 'mqtt_worker'
    callback='MessageArrived' if model=='TC1' else 'command' if model=='A1' else 'native_command'
    extra=lambda names:emitted(frames,names)
    owner=[worker,'ph_mqtt_session_run'];cycle=owner+['session_cycle']
    incoming=owner+['ph_mqtt_poll',callback]
    dispatch=incoming+['service_mqtt_input','service_mqtt_command_mp','mp_dispatch']
    paths={}
    transport=['ph_mqtt_publish','ingress','ph_mqtt_enqueue_route','ph_mqtt_enqueue_generation']
    for name,tail in [('connect',['open_session','ph_mqtt_connect']),('subscribe',['subscribe_plan','ph_mqtt_subscribe']),('yield',['ph_mqtt_yield'])]:
        paths['MQTT '+name]=(owner+tail+transport[1:],reserve)
    paths['MQTT bootstrap']=(owner+extra(['announce','ph_mqtt_discovery_mp','publish_wait'])+transport,reserve)
    paths['MQTT pending receipt']=(owner+['ph_mqtt_poll']+extra(['receipt_retry','publish_owned'])+transport,reserve)
    paths['MQTT result receipt']=(incoming+['service_mqtt_input','ph_mqtt_receipt_send_bytes']+extra(['receipt_retry','publish_owned'])+transport,reserve)
    decode=['phv_parse']+extra(['phv_scan','phv_duplicate','phv_head','phv_skip_validated','binary_token','binary_take','ph_wire_utf8'])
    paths['MQTT input decode']=(incoming+['service_mqtt_input','service_mqtt_command_mp']+extra(['mp_decode'])+decode,reserve)
    for helper in ('service_product_mp','service_product_capabilities_mp','service_product_info_mp'):
        prefix=dispatch+[helper]+extra(['tc1_product_inner_mp'] if model=='TC1' else ['smp_config','smp_control'])
        paths.update(writer_paths(frames,'MQTT '+helper,prefix,reserve))
        paths['MQTT '+helper+' validation']=(prefix+['phv_get']+extra(['phv_scan','phv_iter','phv_next','phv_duplicate','phv_head','phv_skip_validated','binary_token','binary_take']),reserve)
    paths.update(diagnostic_paths(frames,model,'MQTT command diagnostics',dispatch,reserve))
    periodic=cycle+extra(['states_step','states','snapshot_step'])+['service_mqtt_snapshot']+extra(['publish_query'])+['service_query_mp','service_command_mp','mp_dispatch']
    for label,prefix in [('command',dispatch),('periodic',periodic)]:
        paths.update(status_paths(frames,model,'MQTT '+label+' status',prefix+['mp_cached','service_product_status_mp'],reserve))
        paths['MQTT '+label+' cache']=(prefix+['mp_envelope','phw_cached_value']+extra(['phw_prefix','phw_bytes'])+['ph_buf_reserve','ph_wire_malloc'],reserve)
    # HA owns a fixed 1536-byte bridge until its synchronous command returns.
    ha=incoming+extra(['ha_command_input'])
    paths['HA input grammar']=(ha+extra(['ha_command_pack'])+['hac_value']*8+extra(['hac_string','hac_u16','hac_ws'])+['phw_text']+extra(['phw_text_n','phw_prefix','phw_quote','phw_bytes']),reserve)
    paths['HA command business']=(ha+['service_command_mp','mp_dispatch','service_product_mp']+extra(['tc1_product_inner_mp','smp_config','smp_control'])+['phv_get']+extra(['phv_scan','phv_iter','phv_next','phv_duplicate','phv_head','binary_token']),reserve)
    if model=='TC1':
        power=cycle+extra(['snapshot_step'])+['UserMqttHassPower']
        decimal=['ph_decimal_format_float32','pd_format']+extra(['pd_mul','pd_word','pd_bits','pd_shift','pd_cmp'])
        paths['HA TC1 energy decimal']=(power+decimal,reserve)
        paths['HA TC1 energy send']=(power+extra(['UserMqttSendTopic','send_owned','account_message'])+transport,reserve)
    else:
        paths['HA discovery']=(cycle+extra(['discovery','discovery_build','filter_discovery'])+extra(['publish'])+transport,reserve)
    # The HA bridge retains owner scratch while executing; a status request
    # still traverses the same direct sink and must fit without JSON scratch.
    paths.update(status_paths(frames,model,'HA status',ha+['service_command_mp','mp_dispatch','mp_cached','service_product_status_mp'],reserve))
    if model in ('A1','TC1'):
        root=['owner','bemfa_poll']
        paths['Bemfa owned POD dispatch']=(['owner','bemfa_dispatch',model.lower()+'_automation_execute']+extra(['a1_control_apply','feature_fan_apply','install_config','feature_config_save','tc1_config_commit_reserved']),reserve)
        paths['Bemfa input copy']=(root+['bm_poll','bemfa_command']+extra(['bemfa_parse']),reserve)
        paths['Bemfa report capture']=(root+extra(['bemfa_report_one','bemfa_capture'])+[model.lower()+'_status_capture']+extra(['bemfa_config_get','runtime_metrics_capture','feature_fan_snapshot','ph_http_snapshot','tc1_status_begin','tc1_ota_snapshot']),reserve)
        for api in ('connect','subscribe','yield','publish'):
            paths['Bemfa '+api]=(root+extra(['bemfa_report_one'] if api=='publish' else [])+['bm_'+api]+transport[1:],reserve)
    owner_hook = ['ph_mqtt_is_owner', 'current', 'mutex_lock']
    failure = ['ph_mqtt_error', 'ph_mqtt_record_failure'] + owner_hook
    emitted_end = ['end'] if 'end' in frames else []
    read_adapter = {'A1': 'mqtt_socket_read', 'M1': 'm1_mqtt_read', 'TC1': 'tc1_mqtt_read'}[model]
    sites = {
        'client init': owner + ['open_session', 'ph_mqtt_client_init'],
        'connect': owner + ['open_session', 'ph_mqtt_connect'],
        'subscribe': owner + ['subscribe_plan', 'ph_mqtt_subscribe'],
        'yield': owner + ['ph_mqtt_yield'],
        'availability': owner + ['publish_text', 'ph_mqtt_publish'],
        'disconnect': owner + ['close_session', 'ph_mqtt_disconnect'],
    }
    if model == 'M1':
        for label, prefix in (
                ('metadata initial', owner + ['session_initial', 'subscribe_metadata', 'ph_mqtt_subscribe']),
                ('metadata retry', cycle + extra(['retry_metadata']) + ['subscribe_metadata', 'ph_mqtt_subscribe'])):
            sites[label] = prefix
            paths['MQTT '+label] = (prefix + transport[1:], reserve)
    if model in ('A1', 'TC1'):
        paths['Bemfa subscribe rollback'] = (root + ['bm_subscribe', 'ph_mqtt_discard_route'] + owner_hook, reserve)
    # Preserve the retained product/session/sender frames of each audited
    # publication. account_message has returned before TC1 send_owned publishes.
    for label, (names, _) in list(paths.items()):
        if 'ph_mqtt_publish' in names:
            position = names.index('ph_mqtt_publish')
            retained = names[:position] + [n for n in names[position+1:] if n in ('states_step', 'states')]
            retained = [n for n in retained if n != 'account_message']
            if model == 'TC1' and 'MqttMsgPublish' not in retained and 'publish_owned' not in retained: retained += ['MqttMsgPublish']
            sites[label.removeprefix('MQTT ')] = retained + ['ph_mqtt_publish']
    sites['external SEND'] = owner + ['ph_mqtt_poll'] + (extra(['outgoing','send_owned'])+['MqttMsgPublish'] if model == 'TC1' else []) + ['ph_mqtt_publish']
    for label, retained in sites.items():
        paths['MQTT '+label+' error record'] = (retained + emitted_end + failure, reserve)
        if retained[-1] == 'ph_mqtt_subscribe':
            paths['MQTT '+label+' rollback'] = (retained + ['ph_mqtt_discard_route'] + owner_hook, reserve)
        if retained[-1] == 'ph_mqtt_publish':
            paths['MQTT '+label+' admission record'] = (retained + ['ph_mqtt_resource_ready'] + failure, reserve)
        if label != 'disconnect':
            # SDK buffer replacement retains this application admission hook.
            # Owner lookup and live-heap admission are sequential sibling tails;
            # Paho/helper/allocator interiors stay in the unchanged SDK reserve.
            storage = retained + ['mqtt_buffer_allocation_permit']
            paths['MQTT '+label+' buffer owner'] = (storage + ['ph_mqtt_owner','current','mutex_lock'], reserve)
            paths['MQTT '+label+' buffer heap'] = (storage + (['permit'] if 'permit' in frames else []), reserve)
        if label == 'client init': continue  # allocation only, no socket callbacks
        if label == 'disconnect':
            paths['MQTT '+label+' guarded write'] = (retained + ['guarded_write'], reserve)
            continue  # DISCONNECT sends only
        # Admission/drop are exclusive callback tails. The queue attempt has
        # returned before record_ingress_drop enters its short lock.
        paths['MQTT '+label+' ingress drop count'] = (retained + ['ingress', 'ph_mqtt_record_ingress_drop'] + owner_hook, reserve)
        paths['MQTT '+label+' guarded read owner'] = (retained + ['guarded_read', 'ph_mqtt_protocol_buffers'] + owner_hook, reserve)
        paths['MQTT '+label+' guarded read adapter'] = (retained + ['guarded_read', read_adapter], reserve)
        paths['MQTT '+label+' guarded write'] = (retained + ['guarded_write'], reserve)

    # Expand every explicit context wrapper without changing SDK reserve.
    context_frames(names for names,_ in paths.values())
    for label,(names,spare) in paths.items():
        missing=set(names)-frames.keys()
        if missing:raise ValueError(f'{label}: missing actual .su frames {sorted(missing)}')
        size=sum(frames[n] for n in names)
        limit=12288 if label.startswith('Bemfa ') else budget
        if size+spare>limit:raise ValueError(f'{label}: {size}+{spare}>{limit}')
        print(f'{label}: {size} application + {spare} reserve <= {limit} bytes')
