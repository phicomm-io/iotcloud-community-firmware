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
    encode=['json_owned_to_wire','json_write','json_primitive','json_append']
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
        paths['MQTT filter discovery encode']=(cycle+['discovery','filter_discovery','json_owned_publish']+encode,reserve)
    elif model=='M1':
        paths['MQTT feature deferred control']=(owner+['ph_mqtt_poll','feature_command','feature_mqtt_input']+execute+['service_product_inner'],reserve)
        paths['MQTT discovery encode']=(cycle+(['discovery'] if 'discovery' in frames else [])+['json_owned_publish']+encode,reserve)
    else:
        paths['MQTT TC1 legacy control']=(owner+['ph_mqtt_poll','MessageArrived','ProcessHaCmd','UserMqttSendTopic','send_owned','account_message']+transport,reserve)
        # snapshot_step is inlined into session_cycle in the pinned compiler;
        # add it if a future compiler emits a separate frame.
        outer=cycle+(['snapshot_step'] if 'snapshot_step' in frames else [])
        for helper in ('UserMqttHassAuto','UserMqttHassAutoPower','UserMqttHassPower'):
            paths['MQTT TC1 '+helper]=(outer+[helper,'UserMqttSendTopic','send_owned','account_message']+transport,reserve)
    if model!='TC1':paths['MQTT OTA recovery']=(cycle+['ota_job_poll','feature_resume_step'],reserve)
    for label,(names,spare) in paths.items():
        missing=set(names)-frames.keys()
        if missing:raise ValueError(f'{label}: missing actual .su frames {sorted(missing)}')
        size=sum(frames[n] for n in names)
        if size+spare>budget:raise ValueError(f'{label}: {size}+{spare}>{budget}')
        print(f'{label}: {size} application + {spare} reserve <= {budget} bytes')
