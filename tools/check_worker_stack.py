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

def worker_paths(frames,model,binding,symbols=None):
    try:
        from .status_stack import emitted
    except ImportError:
        from status_stack import emitted
    if model not in ('A1','M1'):raise ValueError('Unknown feature worker model: '+model)
    if binding!=12288:raise ValueError('Feature worker budget must remain 12288 bytes')
    extra=lambda names:emitted(frames,names,symbols)
    paths={};worker=['feature_worker'];reserve=2048
    def add(label,names,budget=binding):paths[label]=(budget,reserve,names)
    scheduled=worker+['automation_poll']+extra(['auto_execute'])
    execute=model.lower()+'_automation_execute'
    # Automation now invokes typed controls directly. No service dispatcher or
    # status capture is live under auto_execute. Persisting due records and
    # executing their action are separate tails.
    add('scheduled transaction',worker+['automation_poll','automation_save'])
    if model=='A1':
        callbacks=[('scheduled',scheduled),('button',worker+['a1_input_poll','automation_button']+extra(['auto_execute']))]
        for label,prefix in callbacks:
            control=prefix+[execute,'a1_control_apply']+extra(['control_broadcast','panel_state'])
            add(label+' typed control persistence',control+['feature_fan_apply']+extra(['install_config'])+['feature_config_save'])
            group=control+['feature_group_control_mp']+extra(['group_envelope_mp','group_envelope_token_mp'])+['phw_value','phw_visit']
            add(label+' typed group encode',group+extra(['phw_prefix','phw_bytes'])+['ph_buf_reserve','ph_wire_malloc'])
            add(label+' typed group borrowed traversal',group+['phv_next']+extra(['phv_scan','phv_head','binary_token','ph_wire_utf8']))
            add(label+' light persistence',prefix+[execute,'feature_light_select_mp']+extra(['install_config'])+['feature_config_save'])
            add(label+' filter persistence',prefix+[execute,'a1_filter_change'])
            add(label+' panel inspection',prefix+[execute,'a1_panel_inspect'])
        panel=worker+['a1_input_poll','a1_panel_event','a1_control_apply']
        add('panel typed control',panel+['feature_fan_apply']+extra(['install_config'])+['feature_config_save'])
        add('panel reset transaction',worker+['a1_input_poll','a1_reset','ph_auth_credential_commit']+extra(['a1_reset_commit']))
        add('scheduled countdown transaction',worker+['automation_poll']+extra(['auto_countdown_poll'])+['automation_save'])
    else:
        for leaf in ['m1_set_brightness','m1_reboot_later','m1_wake_alarm_tone_start']:
            add('scheduled '+leaf,scheduled+[execute,leaf])
        add('scheduled control admission',scheduled+[execute,'service_control_begin'])
        add('local CO2 control',worker+['feature_control_co2'])
        # UART task budget is unchanged. Include the actual password/store
        # transaction callback; SDK Flash/RTOS interiors retain 2048 bytes.
        uart=['uart_worker','m1_config_save']+extra(['m1_config_save_language'])
        add('UART config transaction',uart+['ph_auth_credential_commit']+extra(['m1_config_commit']),4096)
        add('UART credential lock',uart+['ph_auth_credential_commit']+extra(['ph_auth_private_lock','local_auth_password']),4096)
        add('UART MCU bridge',['uart_worker','m1_mcu_tick','m1_co2_config_get'],4096)
    parse=worker+['phv_parse']+extra(['phv_scan','phv_duplicate','phv_skip_validated','phv_head','binary_token','binary_take','ph_wire_utf8'])
    add('binding UDP typed parse',parse)
    # The datagram view is borrowed only in this worker iteration. Direct
    # decoding/encoding and synchronous auth complete before recvfrom reuses it;
    # async commands must already own validated data (separate lifecycle tests).
    decode=worker+['lan_decode_value','phv_get']+extra(['phv_iter','phv_next','phv_scan','phv_head'])
    add('binding UDP typed packet',decode)
    for auth in ['ph_auth_pair','ph_auth_pair_resume','ph_auth_pair_unpair_request']:
        add('binding UDP '+auth,worker+['feature_lan_pair',auth]+extra(['ph_auth_private_lock','ph_auth_private_install','ph_auth_private_unpair','ph_auth_private_save','local_auth_save']))
    add('binding UDP response',worker+['feature_lan_pair','phw_text']+extra(['phw_text_n','phw_prefix','phw_quote','phw_bytes']))
    add('binding paired client persist',worker+extra(['lan_client_response','lc_response','lan_client_save'])+['ph_auth_remote_store']+extra(['ph_auth_private_save','local_auth_save']))
    add('binding paired client tick',worker+extra(['lan_client_poll','lc_tick','lc_emit','lan_client_send']))
    add('binding config persistence',worker+extra(['group_input_mp','install_config'])+['feature_config_save'])
    return paths


def read_worker_frames(directory):
    try:
        from .check_http_core import read_frames
    except ImportError:
        from check_http_core import read_frames
    frames=read_frames(directory.parent,directory.name)
    # A .part body is called by its wrapper, unlike alternative .isra clones.
    # Charge both where emitted. Do not lose the auth transaction frame while
    # normalizing GCC's suffix to the public API name.
    entries={};parts={}
    for path in directory.glob('*.su'):
        for line in path.read_text().splitlines():
            location,size,kind=line.split('\t');full=location.rsplit(':',1)[-1];name=full.split('.')[0]
            if '.part.' in full:parts[name]=max(parts.get(name,0),int(size))
            elif '.' not in full:entries[name]=max(entries.get(name,0),int(size))
    for name,size in parts.items():frames[name]=entries.get(name,0)+size
    return frames


def validate_worker_paths(paths,frames):
    for label,(budget,reserve,names) in paths.items():
        if reserve!=2048 or budget not in (4096,12288):raise ValueError('Worker budget/reserve changed: '+label)
        unbounded=set(names)&getattr(frames,'unbounded',{}).keys()
        if unbounded:raise ValueError(label+': unbounded stack usage '+str(sorted(unbounded)))
        missing=set(names)-frames.keys()
        if missing:raise ValueError(label+': missing actual stack usage '+str(sorted(missing)))
        size=sum(frames[name] for name in names)
        if size+reserve>budget:raise ValueError(f'{label}: {size} application + {reserve} reserve > {budget}')
        print(f'{label}: {size} application + {reserve} reserve <= {budget} bytes')


def check(directory,header,nm=None):
    directory=Path(directory);frames=read_worker_frames(directory)
    binding=int(re.search(r'#define FEATURE_WORKER_STACK_SIZE (\d+)u',Path(header).read_text()).group(1))
    # Object symbols establish whether a private helper was inlined. This gate
    # does not claim a linked-ELF singleton check (the HTTP gate owns that).
    import os,subprocess
    try:
        from .check_http_core import linked_symbols
    except ImportError:
        from check_http_core import linked_symbols
    nm=nm or os.environ.get('NM','arm-none-eabi-nm')
    objects=list(directory.glob('*.o'))+list((directory.parent/'phicomm_httpd').glob('*.o'))
    if not objects:raise ValueError('No actual worker objects for inlining audit')
    symbols=linked_symbols(subprocess.check_output([nm,'--defined-only',*map(str,objects)],text=True))
    validate_worker_paths(worker_paths(frames,directory.name,binding,symbols),frames)
    if directory.name=='M1':check_m1_barometer(directory)


if __name__=='__main__':
    try:check(sys.argv[1],sys.argv[2],sys.argv[3] if len(sys.argv)>3 else None)
    except (ValueError,IndexError,AttributeError,OSError) as error:sys.exit(str(error))
