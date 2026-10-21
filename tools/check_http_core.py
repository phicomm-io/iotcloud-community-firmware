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
    # MQTT uses the same owned encoder. Paho can dispatch a command while an
    # outer QoS1 publisher is waiting. Keep those outer application frames,
    # but not the completed response builder, in the encoding call path.
    encode=['json_owned_to_wire','json_write','json_primitive','json_append']
    if 'service_mqtt_wire' in frames:encode+=['service_mqtt_wire']
    if model=='TC1':
        source=(Path(__file__).resolve().parents[1]/'TC1/mqtt_server/user_mqtt_client.c').read_text()
        mqtt_budget=int(re.search(r'mqtt_thread_stack_size = (0x[0-9a-fA-F]+|[0-9]+)',source).group(1),0)
        mqtt_paths={
          'MQTT nested JSON encode':(['MqttClientThread','all_states','UserMqttHassPower','UserMqttSendTopic','MqttMsgPublish','MessageArrived']+encode,3072),
          'MQTT nested status build':(['MqttClientThread','all_states','UserMqttHassPower','UserMqttSendTopic','MqttMsgPublish','MessageArrived','service_command','service_command_execute','service_product','tc1_status_json'],3072),
          'MQTT nested topic lookup':(['MqttClientThread','all_states','UserMqttHassPower','UserMqttSendTopic','MqttMsgPublish','MessageArrived','tc1_command_topic'],3072),
          'MQTT snapshot nested JSON encode':(['MqttClientThread','service_mqtt_publish','service_sender','MqttMsgPublish','MessageArrived']+encode,3072),
          'MQTT flush nested JSON encode':(['MqttClientThread','service_mqtt_flush','service_sender','MqttMsgPublish','MessageArrived']+encode,3072),
        }
    else:
        source=(Path(__file__).resolve().parents[1]/model/'mqtt.c').read_text()
        mqtt_budget=int(re.search(r'mqtt_worker, (\d+), 0',source).group(1))
        callback='command' if model=='A1' else 'native_command'
        # Owned encoding returns before the synchronous publish/Paho callback.
        # Keep each reachable outer branch separate, including its live topic
        # scratch, rather than summing mutually exclusive discovery/telemetry.
        outers={'discovery':['discovery','json_owned_publish']}
        if model=='A1':
            outers['virtual discovery']=['feature_mqtt_service','discovery_virtual','json_owned_publish','checked_publish']
            outers['virtual telemetry']=['feature_mqtt_service','direct_json','json_owned_publish','checked_publish']
            outers['filter discovery']=['discovery','filter_discovery','json_owned_publish']
        mqtt_paths={
          'MQTT flush nested JSON encode':(['mqtt_worker','service_mqtt_flush','publish',callback]+encode,4096),
        }
        for branch,outer in outers.items():
            mqtt_paths['MQTT '+branch+' nested JSON encode']=(['mqtt_worker']+outer+['publish',callback]+encode,4096)
            mqtt_paths['MQTT '+branch+' nested status build']=(['mqtt_worker']+outer+['publish',callback,'service_command','service_command_execute','service_product',status,status+'_view'],4096)
            mqtt_paths['MQTT '+branch+' owned encode']=(['mqtt_worker']+[n for n in outer if n!='checked_publish']+encode,4096)
    for label,(names,reserve) in mqtt_paths.items():
        missing=set(names)-frames.keys()
        if missing:raise ValueError(f'{label}: missing actual .su frames: {sorted(missing)}')
        size=sum(frames[n] for n in names)
        if size+reserve>mqtt_budget:raise ValueError(f'{label}: {size}+{reserve}>{mqtt_budget}')
        print(f'{label}: {size} application + {reserve} reserve <= {mqtt_budget} bytes')
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
