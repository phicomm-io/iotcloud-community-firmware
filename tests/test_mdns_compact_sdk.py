"""Share SDK mDNS error output while retaining its observable log semantics."""
from pathlib import Path
import os
import re
import subprocess
import tempfile
import unittest

from tests.test_native_udp_runtime import function

ROOT=Path(__file__).resolve().parents[1]
SDK=Path(os.environ.get('PHICOMM_LAN_SDK',os.environ.get('PHICOMM_TC1_SDK',
    os.environ.get('TC1_SDK_DIR',ROOT/'build/tc1-sdk-3.6.4/mico-os'))))
REL='MiCO/system/mdns/mico_mdns.c'


class CompactMDNSSDK(unittest.TestCase):
    def test_ordered_overlay_is_idempotent_and_changes_no_protocol_function(self):
        old=subprocess.check_output(['git','-C',str(SDK),'show','HEAD:'+REL],text=True)
        with tempfile.TemporaryDirectory(prefix='compact-mdns-') as td:
            t=Path(td);target=t/REL;target.parent.mkdir(parents=True);target.write_text(old)
            command=['python3',str(ROOT/'tools/apply_sdk_patch.py'),str(t)]
            oom=str(ROOT/'common/bonjour-lan-mdns-oom.patch')
            compact=str(ROOT/'common/bonjour-lan-mdns-compact.patch')
            subprocess.run(command+[oom],check=True,capture_output=True)
            before=target.read_text()
            subprocess.run(command+[oom,compact],check=True,capture_output=True)
            after=target.read_text();stamp=target.stat().st_mtime_ns
            subprocess.run(command+[oom,compact],check=True,capture_output=True)
            self.assertEqual(target.stat().st_mtime_ns,stamp)
            self.assertEqual(target.read_text(),after)
            begin=after.index('/* Share repeated SDK assertion output')
            end=after.index('/** "224.0.0.251" */',begin)
            self.assertEqual(after[:begin]+after[end:],before)
            # In particular, record lifecycle, query/response/announce and all
            # EasyLink-facing entry points remain the actual previous bodies.
            for signature in ('void process_dns_questions(','static int dns_compare_name_to_string(',
                'OSStatus mdns_add_record(','void mdns_suspend_record(',
                'void mdns_resume_record(','void mdns_handler(',
                'void bonjour_send_record(','void _bonjour_thread('):
                self.assertEqual(function(before,signature),function(after,signature))

    def test_actual_pinned_debug_macro_output_and_runtime_disable_are_equal(self):
        debug=(SDK/'include/debug.h').read_text()
        definitions=re.findall(r'^\s*#define debug_print_assert\(A,B,C,D,E,F\) do .*?while\(0==1\)',
                               debug,re.M|re.S)
        self.assertEqual(len(definitions),2)
        patch=(ROOT/'common/bonjour-lan-mdns-compact.patch').read_text()
        addition='\n'.join(line[1:] for line in patch.splitlines()
            if line.startswith('+') and not line.startswith('+++'))
        prelude=r'''
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
static int mico_debug_enabled=1,stdio_tx_mutex,locks,times;
static void mico_rtos_lock_mutex(int *p){assert(!*p);*p=1;locks++;}
static void mico_rtos_unlock_mutex(int *p){assert(*p);*p=0;}
static long mico_rtos_get_time(void){times++;return 42;}
'''
        calls=r'''
static void emit(void){
 debug_print_assert(17,"condition",NULL,"mico_mdns.c",123,"mdns_add_record");
 debug_print_assert(0,NULL,"detail","mico_mdns.c",456,"start_bonjour_service");
}
int main(void){emit();assert(!stdio_tx_mutex);mico_debug_enabled=0;emit();assert(!stdio_tx_mutex);
#ifndef NO_MICO_RTOS
 assert(locks==2&&times==2);
#endif
 return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='mdns-logs-') as td:
            t=Path(td)
            for no_rtos,definition in enumerate(definitions):
                output=[]
                for candidate in (False,True):
                    source=t/'case.c';binary=t/'case'
                    source.write_text(prelude+'\n'+definition+'\n'+(addition if candidate else '')+'\n'+calls)
                    flags=['-DDEBUG=1']+(['-DNO_MICO_RTOS=1'] if no_rtos else [])
                    subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror',
                        '-Wno-unused-function','-Wno-unused-variable',
                        '-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie',
                        *flags,str(source),'-o',str(binary)],check=True,capture_output=True)
                    ran=subprocess.run([str(binary)],capture_output=True,text=True)
                    self.assertEqual(ran.returncode,0,ran.stderr)
                    output.append(ran.stdout)
                self.assertEqual(*output)
                self.assertIn('mico_mdns.c:mdns_add_record: 123',output[0])
                self.assertIn('**ASSERT** detail',output[0])


if __name__=='__main__':unittest.main()
