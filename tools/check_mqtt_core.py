#!/usr/bin/env python3
"""Actual shared-owner application frames; SDK/Paho/RTOS remain in reserve.

Protocol callbacks copy only. No business path is nested below a PUBACK wait.
These are static upper bounds, never runtime stack watermarks.
"""
from pathlib import Path
import re

def check(frames,model):
    root=Path(__file__).resolve().parents[1]
    source=(root/('TC1/mqtt_server/user_mqtt_client.c' if model=='TC1' else model+'/mqtt.c')).read_text()
    budget=int(re.search(r'mqtt_thread_stack_size = (0x[0-9a-fA-F]+|[0-9]+)',source).group(1),0) if model=='TC1' else int(re.search(r'mqtt_worker,\s*(\d+),',source).group(1))
    reserve=3072 if model=='TC1' else 4096
    worker='MqttClientThread' if model=='TC1' else 'mqtt_worker'
    callback='MessageArrived' if model=='TC1' else 'command' if model=='A1' else 'native_command'
    owner=[worker,'ph_mqtt_session_run']
    dispatch=owner+['ph_mqtt_poll',callback,'service_mqtt_input']
    execute=['service_command','service_command_execute','service_product']
    encode=['json_owned_to_wire','json_owned_to_wire_result','json_write','json_primitive','json_append']
    transport=['ph_mqtt_publish','ingress','ph_mqtt_enqueue_route','ph_mqtt_enqueue_generation']
    cycle=owner+['session_cycle']
    paths={
      'MQTT startup binding':([worker,'ph_mqtt_startup_wait','ph_mqtt_owner_init'],reserve),
      'MQTT connect ingress':(owner+['open_session','ph_mqtt_connect']+transport[1:],reserve),
      'MQTT subscribe ingress':(owner+['subscribe_plan','ph_mqtt_subscribe']+transport[1:],reserve),
      'MQTT yield ingress':(owner+['ph_mqtt_yield']+transport[1:],reserve),
      'MQTT deferred result encode':(dispatch+encode,reserve),
      'MQTT result PUBACK ingress':(dispatch+['service_sender' if model=='TC1' else 'publish']+transport,reserve),
      'MQTT deferred control':(dispatch+execute+['service_product_inner'],reserve),
      'MQTT snapshot encode':(cycle+['service_mqtt_snapshot','service_mqtt_publish']+encode,reserve),
      'MQTT capabilities encode':(cycle+['service_mqtt_snapshot','service_mqtt_try_capabilities']+encode,reserve),
      'MQTT snapshot PUBACK ingress':(cycle+['service_mqtt_snapshot','service_mqtt_publish','service_sender' if model=='TC1' else 'publish']+transport,reserve),
    }
    if model=='A1':
        for label in ('MQTT snapshot encode','MQTT snapshot PUBACK ingress','MQTT capabilities encode'):
            paths[label][0].extend(n for n in ('states_step','states') if n in frames)
    # Status capture and encoding are sequential tails. Never charge the
    # unrelated service_product_inner configuration scratch on a status path.
    status=model.lower()+'_status_json'
    common=dispatch+execute+[status,status+'_view',model.lower()+'_status_full']
    # Periodic telemetry invokes the same builder directly, outside command
    # dispatch; include its retained cycle/leaf frames as a separate path.
    leaf = [n for n in ('states_step', 'states') if n in frames] if model=='A1' else []
    telemetry=cycle+leaf+['service_mqtt_snapshot','service_query']+execute+[status,status+'_view',model.lower()+'_status_full']
    captures=[model.lower()+'_status_capture']+[n for n in ('runtime_metrics_capture','feature_fan_snapshot','watchdog_status_capture','a1_filter_get','ph_http_snapshot','tc1_status_begin','tc1_ota_snapshot','reset_name','device_status_text') if n in frames]
    pure=['device_status_encode','encode_'+model.lower()]+[n for n in ('device_runtime_encode','device_http_encode','device_filter_encode','watchdog_status_encode') if n in frames and model!='TC1']
    paths['MQTT status capture']=(common+captures,reserve)
    paths['MQTT status pure encode']=(common+pure,reserve)
    paths['MQTT periodic status capture']=(telemetry+captures,reserve)
    paths['MQTT periodic status pure encode']=(telemetry+pure,reserve)
    if model=='A1':
        paths['MQTT deferred control'][0].extend(['a1_control_local','feature_group_control','group_send','parse'])
        paths['MQTT feature deferred control']=(owner+['ph_mqtt_poll','feature_command','feature_mqtt_input']+execute+['service_product_inner','a1_control_local','feature_group_control','group_send','parse'],reserve)
        virtual=cycle+['feature_mqtt_service','virtual_service']
        paths['MQTT virtual discovery encode']=(virtual+['discovery_virtual_step','json_owned_publish']+encode,reserve)
        paths['MQTT virtual telemetry encode']=(virtual+[n for n in ('direct_json_claimed',) if n in frames]+encode,reserve)
        paths['MQTT virtual telemetry ingress']=(virtual+(['direct_leaf'] if 'direct_leaf' in frames else [])+['checked_publish','publish']+transport,reserve)

    elif model=='M1':
        paths['MQTT feature deferred control']=(owner+['ph_mqtt_poll','feature_command','feature_mqtt_input']+execute+['service_product_inner'],reserve)

    else:
        paths['MQTT TC1 legacy control']=(owner+['ph_mqtt_poll','MessageArrived','ProcessHaCmd','UserMqttSendTopic','send_owned','account_message']+transport,reserve)
        # snapshot_step is inlined into session_cycle in the pinned compiler;
        # add it if a future compiler emits a separate frame.
        outer=cycle+(['snapshot_step'] if 'snapshot_step' in frames else [])
        for helper in ('UserMqttHassAuto','UserMqttHassAutoPower','UserMqttHassPower'):
            paths['MQTT TC1 '+helper]=(outer+[helper,'UserMqttSendTopic','send_owned','account_message']+transport,reserve)
    # Native optional graphs acquire/release their own admission scope. Static
    # product wrappers and finish can inline; externally visible APIs and the
    # allocator hook must be represented by actual emitted frames (even zero).
    # SDK allocator/RTOS interiors stay in the unchanged reserve; explicitly add
    # the new application hook nested below encoding/build allocation calls.
    optional_sources = {}
    finish = ['service_mqtt_json_finish'] if 'service_mqtt_json_finish' in frames else []
    if model != 'TC1':
        native = cycle + [n for n in ('discovery', 'discovery_build') if n in frames]
        optional_sources['discovery'] = native
        if model == 'A1':
            optional_sources['filter discovery'] = native + ['filter_discovery']
        paths['MQTT discovery admission'] = (cycle + ([ 'discovery' ] if 'discovery' in frames else []) + ['service_mqtt_json_begin'], reserve)
        paths['MQTT discovery final cleanup'] = (cycle + ([ 'discovery' ] if 'discovery' in frames else []) + ['service_mqtt_json_end'] + finish, reserve)
        for label, retained in optional_sources.items():
            encoder = retained + ['service_mqtt_optional_send', 'json_owned_to_wire_result', 'json_write', 'json_primitive', 'json_append']
            paths['MQTT '+label+' encode'] = (encoder, reserve)
            paths['MQTT '+label+' allocation observation'] = (encoder + ['json_heap_failure'], reserve)
            paths['MQTT '+label+' graph allocation observation'] = (retained + ['json_heap_failure'], reserve)
            paths['MQTT '+label+' graph cleanup'] = (retained + ['service_mqtt_json_error'] + finish, reserve)
            paths['MQTT '+label+' encoding cleanup'] = (retained + ['service_mqtt_optional_send'] + finish, reserve)
            paths['MQTT '+label+' PUBACK ingress'] = (retained + ['service_mqtt_optional_send', 'publish'] + transport, reserve)
    if model!='TC1':paths['MQTT OTA recovery']=(cycle+['ota_job_poll','feature_resume_step'],reserve)
    # Protocol error recording runs AFTER Paho returns. Adapter callbacks run
    # WITH Paho live (covered by the unchanged SDK reserve), but are application
    # frames themselves. Never add the ingress/JSON encoding sibling tails.
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
    # Preserve the retained product/session/sender frames of each audited
    # publication. account_message has returned before TC1 send_owned publishes.
    for label, (names, _) in list(paths.items()):
        if 'ph_mqtt_publish' in names:
            position = names.index('ph_mqtt_publish')
            retained = names[:position] + [n for n in names[position+1:] if n in ('states_step', 'states')]
            retained = [n for n in retained if n != 'account_message']
            if model == 'TC1': retained += ['MqttMsgPublish']
            sites[label.removeprefix('MQTT ')] = retained + ['ph_mqtt_publish']
    sites['external SEND'] = owner + ['ph_mqtt_poll'] + (['outgoing', 'send_owned', 'MqttMsgPublish'] if model == 'TC1' else []) + ['ph_mqtt_publish']
    if model == 'A1':
        sites['virtual discovery'] = virtual + ['discovery_virtual_step', 'json_owned_publish', 'checked_publish', 'publish', 'ph_mqtt_publish']
        # Native optional publication sites were derived from PUBACK paths above.
    elif model == 'M1':
        pass  # Native optional sites above already account for emitted wrappers.
    for label, retained in sites.items():
        paths['MQTT '+label+' error record'] = (retained + emitted_end + failure, reserve)
        if retained[-1] == 'ph_mqtt_publish':
            paths['MQTT '+label+' admission record'] = (retained + failure, reserve)
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
    # Immediate application-failure markers run after encoding/allocation helpers
    # return; do not retain their mutually exclusive frames. SDK failures still
    # record via the independent protocol tails above.
    app_marker = ['ph_mqtt_app_error', 'ph_mqtt_record_operation'] + owner_hook
    app_sites = {'receipt admission': owner + ['ph_mqtt_poll']}
    if model != 'TC1':
        for label, retained in optional_sources.items():
            app_sites[label+' graph'] = retained + ['service_mqtt_json_error']
            app_sites[label+' identity'] = retained
            app_sites[label+' encoding'] = retained + ['service_mqtt_optional_send']
    else:
        app_sites['send conversion'] = owner + ['ph_mqtt_poll', 'outgoing']
        # Void legacy command replies ignore conversion errors; only synchronous
        # snapshot publication and external SEND have fatal application returns.
        for helper in ('UserMqttHassAuto', 'UserMqttHassAutoPower', 'UserMqttHassPower',
                       'UserMqttHassAutoLed', 'UserMqttHassAutoTotalSocket',
                       'UserMqttHassAutoChildLock', 'UserMqttHassAutoRebootButton',
                       'UserMqttHassAutoSoftRebootButton', 'UserMqttSendSocketState',
                       'UserMqttSendLedState', 'UserMqttSendTotalSocketState',
                       'UserMqttSendChildLockState'):
            app_sites['TC1 '+helper] = outer + [helper, 'UserMqttSendTopic', 'send_owned']
    # ota_service/ota_emit can inline into the product cycle. Charge each only
    # if emitted, while the marker itself and its lock helper are required.
    app_sites['OTA graph or encoding'] = cycle + [n for n in ('ota_service', 'ota_emit') if n in frames]
    for label, retained in app_sites.items():
        paths['MQTT '+label+' source marker'] = (retained + app_marker, reserve)
    for label, (names, spare) in list(paths.items()):
        if 'json_owned_to_wire' in names:
            paths[label+' allocation observation'] = (names + ['json_heap_failure'], spare)
    # Actual compiled patched json_heap.c frames are required, separately from
    # the strong application hook. permit (guard) and created (malloc NULL)
    # are sibling tails, not nested. realloc(NULL) can nest malloc; include it
    # conservatively for that API. RTOS is_current_thread internals remain SDK.
    for label, (names, spare) in list(paths.items()):
        if not label.endswith('allocation observation'): continue
        for api in ('json_heap_malloc', 'json_heap_calloc', 'json_heap_realloc', 'json_heap_strdup'):
            allocator = [api] + (['json_heap_malloc'] if api == 'json_heap_realloc' else [])
            for tail in ('permit', 'created'):
                paths[label+' '+api+' '+tail] = (names[:-1] + allocator + ['json_heap.'+tail, 'json_heap_failure'], spare)
    paths['MQTT network error record'] = (owner + ['open_session'] + failure, reserve)
    paths['MQTT session error record'] = (owner + failure, reserve)
    for label,(names,spare) in paths.items():
        missing=set(names)-frames.keys()
        if missing:raise ValueError(f'{label}: missing actual .su frames {sorted(missing)}')
        size=sum(frames[n] for n in names)
        if size+spare>budget:raise ValueError(f'{label}: {size}+{spare}>{budget}')
        print(f'{label}: {size} application + {spare} reserve <= {budget} bytes')
