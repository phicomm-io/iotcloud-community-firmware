#!/usr/bin/env python3
"""Actual LAN owner paths; product-bounded tasks and 2 KiB SDK/crypto reserve.

This is a conservative static bound, not a physical button/RTOS watermark.
Capture and encoding are sequential tails of one owned response construction.
"""
from pathlib import Path
import re
import subprocess
import sys
try:
 from .status_stack import emitted,status_paths,diagnostic_paths,writer_paths,feature_read_paths
 from .check_worker_stack import read_worker_frames
 from .check_http_core import linked_symbols,dispatch_frames,receipt_paths,tc1_product_paths
except ImportError:
 from status_stack import emitted,status_paths,diagnostic_paths,writer_paths,feature_read_paths
 from check_worker_stack import read_worker_frames
 from check_http_core import linked_symbols,dispatch_frames,receipt_paths,tc1_product_paths
LAN_BUDGET=12288
LAN_RESERVE=2048
SHA_UTIL_FRAMES={
 'SHA256Reset','SHA256Input','SHA256Result',
 'SHA224_256ProcessMessageBlock','SHA224_256PadMessage','SHA224_256ResultN.part.0',
}

def add_sha_utils_frames(frames,modules,symbols):
 path=Path(modules)/'mico-os/MiCO/security/SHAUtils/sha224-256.su'
 linked=SHA_UTIL_FRAMES&symbols.keys()
 if not linked:return
 if not path.is_file():raise ValueError('Missing actual SDK SHAUtils .su file: '+str(path))
 found={}
 for line in path.read_text().splitlines():
  location,size,kind=line.split('\t')
  name=location.rsplit(':',1)[-1]
  if name in linked:found[name]=(int(size),kind)
 missing=linked-found.keys()
 if missing:raise ValueError('Emitted SHAUtils helper missing actual .su frame: '+str(sorted(missing)))
 for name,(size,kind) in found.items():
  frames[name]=size
  if kind!='static':frames.unbounded[name]=kind

def lan_paths(frames,model,symbols=None):
 if model not in ('A1','M1','TC1'):raise ValueError('Unknown LAN model: '+model)
 extra=lambda names:emitted(frames,names,symbols)
 worker=['tc1_lan_worker' if model=='TC1' else 'feature_worker']
 adapter=worker+extra(['lan_business_input'])
 server=adapter+['lan_v4_server_input']
 service=server+extra(['lan_business_dispatch'])+['service_lan_try_command_mp']
 dispatch=service+dispatch_frames(frames,symbols)
 paths={}
 def add(label,names):paths['LAN '+label]=(names,LAN_RESERVE)
 scan=extra(['phv_iter','phv_child_reader','phv_next','phv_scan','phv_duplicate','phv_skip_validated','phv_head','binary_token','binary_take','ph_wire_utf8'])
 add('outer parse',worker+['phv_parse']+scan)
 add('v4 decode',adapter+['lan_v4_decode_value','phv_get']+scan)
 add('request parse',service+extra(['mp_decode'])+['phv_parse']+scan)
 add('request schema',service+extra(['mp_decode'])+['phv_keys']+scan)
 auth=server+extra(['lan_business_authorize'])+['feature_lan_context','ph_auth_udp_check']
 add('authorize',auth+extra(['ph_auth_private_lock','ph_auth_private_alive']))
 for prefix,label in [(server+extra(['lan_business_valid']),'context'),(service+extra(['service_lan_context_valid']),'pre-dispatch context'),(dispatch+extra(['service_lan_context_valid']),'in-command context')]:
  add(label,prefix+['ph_auth_context_valid']+extra(['ph_auth_private_lock','ph_auth_private_alive']))
 add('nonblocking admission',server+extra(['lan_business_dispatch'])+['service_memory_try_claim'])
 add('command gate',dispatch+extra(['service_command_enter']))
 sha=extra(SHA_UTIL_FRAMES)
 add('body digest',server+extra(['lan_business_digest'])+sha)
 add('receipt namespace digest',service+sha)
 add('receipt digest',dispatch+sha)
 paths.update(status_paths(frames,model,'LAN full status',dispatch+['mp_cached','service_product_status_mp'],LAN_RESERVE,symbols=symbols))
 paths.update(feature_read_paths(frames,model,'LAN feature config',dispatch,LAN_RESERVE,symbols))
 paths.update(diagnostic_paths(frames,model,'LAN diagnostics',dispatch,LAN_RESERVE,symbols))
 for helper in (('service_product_capabilities_mp','service_product_info_mp') if model=='TC1' else ('service_product_capabilities_mp','service_product_info_mp','service_product_mp')):
  paths.update(writer_paths(frames,'LAN '+helper,dispatch+[helper],LAN_RESERVE,symbols))
 paths.update(writer_paths(frames,'LAN envelope',dispatch+['mp_envelope'],LAN_RESERVE,symbols))
 add('cached envelope',dispatch+['mp_envelope','phw_cached_value']+extra(['phw_prefix','phw_head','phw_bytes'])+['ph_buf_reserve','ph_wire_malloc'])
 paths.update(receipt_paths(frames,'LAN receipt',dispatch,LAN_RESERVE,symbols))
 business=dispatch+['service_product_mp']+extra(['smp_control','smp_music','smp_text','smp_int','smp_has'])
 if model=='TC1':paths.update(tc1_product_paths(frames,'LAN business',dispatch,LAN_RESERVE,symbols))
 else:add('business validation',business+['phv_get']+scan)
 if model=='A1':
  control=dispatch+['service_product_mp']+extra(['smp_control'])+['a1_control_apply']+extra(['control_broadcast','panel_state','a1_control_light_percent'])
  add('control persistence',control+['feature_fan_apply']+extra(['install_config'])+['feature_config_save'])
  add('light persistence',dispatch+['service_product_mp','feature_mp','feature_light_select_mp']+extra(['install_config'])+['feature_config_save'])
 fragment=server+extra(['lv4_emit'])+['lan_v4_encode']
 for api in ['phw_text','phw_u32','phw_bytes_value']:
  add('fragment '+api,fragment+[api]+extra(['phw_text_n','phw_uint','phw_prefix','phw_head','phw_bytes']))
 add('fragment fixed init',server+extra(['lv4_emit'])+['phw_init_fixed'])
 add('fragment send',server+extra(['lv4_emit','lan_business_send']))
 add('response release',server+['lan_v4_server_release','ph_buf_free','ph_wire_free'])
 for api in ['ph_auth_pair','ph_auth_pair_resume','ph_auth_pair_resume_boot','ph_auth_pair_unpair_request']:
  add('pair '+api,worker+['feature_lan_pair',api]+extra(['ph_auth_private_lock','ph_auth_private_install','ph_auth_private_unpair','ph_auth_private_save','local_auth_save']))
 add('pair response',worker+['feature_lan_pair','phw_text']+extra(['phw_text_n','phw_prefix','phw_bytes']))
 # Start is a sibling of packet handling. Charge all emitted safe mDNS builder
 # frames; SDK mdns/socket/thread internals remain within the same reserve.
 add('mDNS registration',worker+extra(['lan_business_start','bonjour_lan_start'])+['bonjour_lan_txt']+extra(['bonjour_lan_field']))
 return paths

def validate_lan_paths(paths,frames,budget=LAN_BUDGET):
 if budget not in (8192,LAN_BUDGET):raise ValueError('LAN task budget must be 8192 or 12288 bytes')
 for label,(names,reserve) in paths.items():
  if reserve!=LAN_RESERVE:raise ValueError('LAN SDK/crypto reserve must remain 2048 bytes')
  missing=set(names)-frames.keys()
  if missing:raise ValueError(label+': missing actual .su frames '+str(sorted(missing)))
  unbounded=set(names)&getattr(frames,'unbounded',{}).keys()
  if unbounded:raise ValueError(label+': unbounded actual stack '+str(sorted(unbounded)))
  size=sum(frames[n] for n in names)
  if size+reserve>budget:raise ValueError(f'{label}: {size}+{reserve}>{budget}')
  print(f'{label}: {size} application + {reserve} reserve <= {budget} bytes')

def check(modules,model,nm,elf):
 root=Path(__file__).resolve().parents[1]
 header=root/('TC1/lan.h' if model=='TC1' else 'common/phicomm_features.h')
 macro='TC1_LAN_STACK_SIZE' if model=='TC1' else 'FEATURE_WORKER_STACK_SIZE'
 budget=int(re.search(r'#define '+macro+r' (\d+)u',header.read_text()).group(1))
 if budget!=(8192 if model=='TC1' else LAN_BUDGET):raise ValueError('Unexpected product LAN task budget')
 frames=read_worker_frames(Path(modules)/model)
 symbols=linked_symbols(subprocess.check_output([nm,'--defined-only',elf],text=True))
 add_sha_utils_frames(frames,Path(modules),symbols)
 for api in ('lan_v4_server_input','lan_v4_decode_value','service_lan_try_command_mp','ph_auth_pair_resume_boot','bonjour_lan_txt'):
  if symbols.get(api)!=1:raise ValueError('Expected one actual linked LAN entry: '+api)
 validate_lan_paths(lan_paths(frames,model,symbols),frames,budget)
if __name__=='__main__':
 try:check(*sys.argv[1:])
 except (ValueError,IndexError,OSError) as error:sys.exit(str(error))
