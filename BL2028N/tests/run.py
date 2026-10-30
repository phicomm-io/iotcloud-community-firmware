#!/usr/bin/env python3
"""Sanitized production business core and actual shared socket services."""
import argparse,os,shutil,subprocess,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from render_web import render,header
from source_provenance import verify_sources
HERE=Path(__file__).resolve().parent;ROOT=HERE.parent

def main():
 verify_sources()
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--run-dir',type=Path,required=True);p.add_argument('--stock-backup',type=Path);a=p.parse_args();run=a.run_dir.resolve();run.mkdir(parents=True,exist_ok=True)
 clang=shutil.which('clang');assert clang
 flags=[clang,'-std=c99','-g','-O1','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-Wno-sign-compare','-fsanitize=address,undefined','-fno-omit-frame-pointer']
 def command(argv,name):
  with (run/name).open('w') as log:subprocess.run(list(map(str,argv)),stdout=log,stderr=subprocess.STDOUT,check=True)
 core=[ROOT/'src'/('ph_'+n+'.c') for n in ('device','meter','udp','wire')]+[ROOT/'protocol/local_auth.c']
 command(flags+[HERE/'host.c']+core+['-lm','-o',run/'host-tests'],'host-compiler.log')
 command([sys.executable,HERE/'test_protocols.py',run/'host-tests'],'host-tests.log')
 command(flags+[HERE/'cache.c']+core+['-Wl,--wrap=malloc,--wrap=free','-lm','-o',run/'cache-test'],'cache-compiler.log')
 command([run/'cache-test'],'cache.log')
 command([sys.executable,HERE/'test_http_auth.py','--run-dir',run/'http-auth'],'http-auth.log')
 command([sys.executable,HERE/'test_http_auth_port.py','--run-dir',run/'http-auth-port'],'http-auth-port.log')
 command([sys.executable,HERE/'test_sha256.py','--run-dir',run/'sha256'],'sha256.log')
 command([sys.executable,HERE/'test_lan_v4.py','--run-dir',run/'lan-v4'],'lan-v4.log')
 command([sys.executable,HERE/'test_mdns.py','--run-dir',run/'mdns'],'mdns.log')
 if a.stock_backup:
  command(flags+['-I',ROOT/'src',HERE/'factory_mac.c','-o',run/'factory-mac-test'],'factory-compiler.log')
  command([run/'factory-mac-test',a.stock_backup],'factory-mac.log')
 html=render();(run/'web.html').write_bytes(html);(run/'web.h').write_text(header(html))
 (run/'sys_config.h').write_text('/* Host SHA256-only SDK config facade. */\n')
 sources=[HERE/'network.c',HERE/'platform_host.c']+list((ROOT/'src').glob('ph_*.c'))
 sources=[s for s in sources if s.name!='ph_identity.c']
 for directory in ('protocol','protocol/httpd','protocol/mqtt','protocol/paho','protocol/paho/MQTTPacket'):sources+=list((ROOT/directory).glob('*.c'))
 sdk=Path.home()/'.cache/phicomm/openbeken/bk7231n-upstream/platforms/bk7231n/bk7231n_os/beken378/func/wolfssl'
 host_flags=['-D_DEFAULT_SOURCE','-DPH_HOST_TEST','-DPH_S30_LAN_V4','-DPH_HTTP_PORT=ph_host_http_port','-I',HERE,'-I',ROOT/'src','-I',run,'-I',sdk,'-Wno-unused-parameter','-Wno-missing-field-initializers','-Wno-unused-variable','-Wno-unused-function','-Wno-unused-but-set-variable','-Wno-parentheses-equality']
 lan=[ROOT/'src'/name for name in ('ph_lan_v4.c','ph_mdns.c','ph_sha256.c')]
 command(flags+host_flags+[HERE/'migration.c',HERE/'platform_host.c']+core+lan+['-lm','-lpthread','-o',run/'migration-test'],'migration-compiler.log')
 command([run/'migration-test',run/'migration-storage'],'migration.log')
 command(flags+host_flags+[HERE/'wifi.c',HERE/'platform_host.c']+core+lan+['-Wl,--wrap=rtos_get_time,--wrap=xPortGetFreeHeapSize,--wrap=bk_wlan_scan_ap_reg_cb,--wrap=bk_wlan_start_scan,--wrap=sr_get_scan_results,--wrap=bk_wlan_get_scan_ap_result_numbers,--wrap=sr_release_scan_results,--wrap=CFG_Save_IfThereArePendingChanges','-lm','-lpthread','-o',run/'wifi-test'],'wifi-compiler.log')
 command([run/'wifi-test'],'wifi.log')
 command(flags+host_flags+[HERE/'meter_uart.c',HERE/'platform_host.c']+core+lan+['-lm','-lpthread','-o',run/'meter-uart-test'],'meter-uart-compiler.log')
 for safe in (0,1):
  storage=run/f'meter-uart-storage-{safe}';shutil.rmtree(storage,ignore_errors=True)
  command([run/'meter-uart-test',storage,safe],f'meter-uart-{safe}.log')
 command(flags+host_flags+sources+['-lm','-lpthread','-o',run/'network-tests'],'shared-compiler.log')
 suback=[s for s in sources if s.name not in ('network.c','client.c')]
 command(flags+host_flags+[HERE/'mqtt_suback.c']+suback+['-Wl,--wrap=malloc','-lm','-lpthread','-o',run/'mqtt-suback-tests'],'mqtt-suback-compiler.log')
 command([run/'mqtt-suback-tests'],'mqtt-suback.log')
 command(flags+['-funsigned-char']+host_flags+[HERE/'mqtt_suback.c']+suback+['-Wl,--wrap=malloc','-lm','-lpthread','-o',run/'mqtt-suback-unsigned-tests'],'mqtt-suback-unsigned-compiler.log')
 command([run/'mqtt-suback-unsigned-tests'],'mqtt-suback-unsigned.log')
 shutil.rmtree(run/'host-storage',ignore_errors=True)
 command([sys.executable,HERE/'test_network.py',run/'network-tests',run],'network-tests.log')
 command(flags+host_flags+sources+[HERE/'mqtt_heap.c','-Wl,--wrap=malloc,--wrap=calloc,--wrap=free,--wrap=realloc,--wrap=xPortGetFreeHeapSize,--wrap=ph_mqtt_receipt_send_bytes','-lm','-lpthread','-o',run/'mqtt-heap-tests'],'mqtt-heap-compiler.log')
 command([sys.executable,HERE/'test_mqtt_heap.py',run/'mqtt-heap-tests',run/'mqtt-heap'],'mqtt-heap.log')
 command([sys.executable,HERE/'test_mqtt_optional.py',run/'mqtt-heap-tests',run/'mqtt-optional'],'mqtt-optional.log')
 if os.environ.get('PHICOMM_PLAYWRIGHT_MODULE'):
  command([sys.executable,HERE/'test_browser.py',run/'network-tests',run],'browser-tests.log')
 print(f'PASS: sanitized core and actual shared HTTP/Paho MQTT/UDP. Evidence: {run}')

if __name__=='__main__':main()
