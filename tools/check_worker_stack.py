#!/usr/bin/env python3
"""Conservative application-frame budgets for synchronous task call paths.

Pinned GCC .su data plus reserves for SDK/JSON/RTOS; not runtime watermarks.
MQTT owner paths are checked separately with their compiled shared archive.
"""
import re
import sys
from pathlib import Path


def check_m1_barometer(directory):
    """Bound the serial hardware owner's new tail without changing its stack.

    Sum every emitted Bosch/adapter frame, even mutually exclusive/FIFO-only
    ones, rather than assume a short init or compensation call chain. There is
    no recursion. Active I2C calls read at most the 21-byte calibration block
    and write one register; round the vendor VLAs up to ARM's 8-byte alignment.
    The 2048-byte SDK/RTOS reserve includes I2C, sleep and mutex callees.
    """
    frames = {}
    for path in directory.rglob('*.su'):
        if path.name not in ('uart.su', 'barometer.su', 'bmp3.su'):
            continue
        for line in path.read_text().splitlines():
            location, size, kind = line.split('\t')
            name = location.rsplit(':', 1)[-1].split('.')[0]
            if path.name == 'uart.su' and name != 'uart_worker':
                continue
            extra = {'bmp3_get_regs': 24, 'bmp3_set_regs': 8}.get(name, 0)
            if kind != 'static' and not extra:
                raise ValueError('Unbounded hardware frame: ' + name)
            frames[name] = max(frames.get(name, 0), int(size) + extra)
    required = {'uart_worker', 'm1_barometer_tick', 'read_regs', 'write_regs',
                'bmp3_init', 'bmp3_set_op_mode', 'bmp3_get_sensor_data',
                'bmp3_get_regs', 'bmp3_set_regs', 'compensate_pressure'}
    if required - frames.keys():
        raise ValueError('Missing hardware stack usage: ' + ', '.join(sorted(required - frames.keys())))
    source = (Path(__file__).resolve().parents[1]/'M1/uart.c').read_text()
    budget = int(re.search(r'"m1_uart", uart_worker, (\d+),', source).group(1))
    application, reserve = sum(frames.values()), 2048
    if application + reserve > budget:
        raise ValueError(f'M1 barometer hardware: {application} application + {reserve} reserve > {budget}')
    print(f'M1 barometer hardware: {application} application + {reserve} reserve <= {budget} bytes')


def check(directory, header):
    directory = Path(directory)
    frames = {}
    for path in list(directory.glob('*.su')) + list((directory.parent/'phicomm_httpd').glob('*.su')):
        for line in path.read_text().splitlines():
            location, size, kind = line.split('\t')
            name = location.rsplit(':', 1)[-1].split('.')[0]
            if kind == 'static':
                frames[name] = max(frames.get(name, 0), int(size))
    binding = int(re.search(r'#define FEATURE_WORKER_STACK_SIZE (\d+)u',
                            Path(header).read_text()).group(1))
    paths = {
        'scheduled control': (binding, 2048, ['feature_worker', 'automation_poll',
            'auto_execute', 'service_command', 'service_product', 'service_product_inner']),
        'scheduled status': (binding, 2048, ['feature_worker', 'automation_poll',
            'auto_execute', 'service_query', 'service_command', 'service_product',
            'a1_status_json' if directory.name == 'A1' else 'm1_status_json',
            'a1_status_json_view' if directory.name == 'A1' else 'm1_status_json_view']),
    }
    status='a1_status_json' if directory.name=='A1' else 'm1_status_json'
    if directory.name=='A1':
        paths={}
        for label,prefix in (
            ('scheduled control',['feature_worker','automation_poll']),
            ('button control',['feature_worker','a1_input_poll','automation_button']),
            ('panel slider',['feature_worker','a1_input_poll','a1_panel_event'])):
            local=prefix+(['a1_automation_execute','a1_panel_inspect'] if label!='panel slider' else [])+['a1_control_apply']
            local += [n for n in ('auto_execute','control_broadcast') if n in frames]
            paths[label+' group delivery']=(binding,2048,local+['feature_group_control','group_send','parse'])
            paths[label+' feature persistence']=(binding,2048,local+['feature_fan_apply','install_config','feature_config_save'])
        paths['button light effect']=(binding,2048,['feature_worker','a1_input_poll','automation_button',
            'a1_automation_execute','feature_light_select','install_config','feature_config_save'])
        paths['panel recovery']=(binding,2048,['feature_worker','a1_input_poll','a1_reset'])
        paths['button filter persistence']=(binding,2048,['feature_worker','a1_input_poll','automation_button',
            'a1_automation_execute','a1_filter_change'])
    # MQTT paths are checked with the linked shared archive in check_http_core.
    expanded={}
    for label,(budget,reserve,names) in paths.items():
        if status not in names:
            expanded[label]=(budget,reserve,names);continue
        common=names+[directory.name.lower()+'_status_full']
        expanded[label+' capture']=(budget,reserve,common+[directory.name.lower()+'_status_capture']+[n for n in ('runtime_metrics_capture','feature_fan_snapshot','watchdog_status_capture','a1_filter_get','ph_http_snapshot','tc1_status_begin','tc1_ota_snapshot','reset_name') if n in frames])
        encode=['device_status_encode','encode_'+directory.name.lower()]
        encode += [n for n in ('device_runtime_encode','device_http_encode','device_filter_encode','watchdog_status_encode') if n in frames]
        encode += [n for n in ('status_bool',) if n in frames]
        expanded[label+' encode']=(budget,reserve,common+encode)
    # Attribution executes after the business builder returns. Check its own
    # tail rather than charging it together with the full snapshot scratch.
    for label,(_,_,names) in paths.items():
        if 'service_command' not in names:continue
        prefix=names[:names.index('service_command')+1]
        expanded[label+' failure attribution']=(binding,2048,prefix+['service_caller_graph_failed','service_memory_failures','json_heap_lock'])
    from check_http_core import read_json_allocator_frames
    from json_numeric_stack import read_numeric_frames, numeric_tails
    read_json_allocator_frames(directory.parent,frames)
    read_numeric_frames(directory.parent,frames)
    for label,(budget,reserve,names) in list(expanded.items()):
        if 'parse' in names:
            prefix=names[:names.index('parse')+1]+['json_tokener_parse_ex']
            for branch,tail in numeric_tails(frames,'scan').items():
                expanded[label+' numeric '+branch]=(budget,reserve,prefix+tail)
    # UDP binding input is parsed directly on both product feature workers,
    # independently of the scheduled A1 group-control path above.
    for branch,tail in numeric_tails(frames,'scan').items():
        expanded['binding UDP numeric '+branch]=(binding,2048,
            ['feature_worker','parse','json_tokener_parse_ex']+tail)
    paths=expanded
    for label, (budget, reserve, names) in paths.items():
        missing = set(names) - frames.keys()
        if missing:
            raise ValueError('Missing stack usage: ' + ', '.join(sorted(missing)))
        application = sum(frames[name] for name in names)
        if application + reserve > budget:
            raise ValueError(f'{label}: {application} application + {reserve} reserve > {budget}')
        print(f'{label}: {application} application + {reserve} reserve <= {budget} bytes')
    if directory.name == 'M1':
        check_m1_barometer(directory)


if __name__ == '__main__':
    try:
        check(sys.argv[1], sys.argv[2])
    except (ValueError, IndexError, AttributeError, OSError) as error:
        sys.exit(str(error))
