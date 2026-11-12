"""TC1 production HTTP/core/service/control paths with deterministic heap faults."""
from pathlib import Path
import os
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class ControlCredit(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='tc1-control-credit-')
        temp = Path(cls.temp.name)
        (temp / 'generated').mkdir()
        (temp / 'generated/build_ota.h').write_text('')
        cls.exe = temp / 'credit'
        sources = ['tests/tc1_control_credit.c', 'common/service.c',
                   'common/httpd/product.c', 'common/httpd/core.c',
                   'common/httpd/parser.c', 'common/typed_wire.c', 'common/wire_buffer.c']
        command = [os.environ.get('CC', 'cc'), '-std=gnu11', '-DMOC=1',
                   '-DPHICOMM_STATUS_MODEL=2', '-g', '-Wall', '-Wextra', '-Werror',
                   '-Wno-unused-function', '-Wno-unused-variable', '-Wno-misleading-indentation',
                   '-Wno-deprecated-declarations', '-fsanitize=address,undefined',
                   '-fno-omit-frame-pointer', '-no-pie', '-pthread',
                   '-I'+str(ROOT / 'tests/service_mp'), '-I'+str(temp), '-I'+str(ROOT),
                   *[str(ROOT / source) for source in sources],
                   '-Wl,--wrap=malloc', '-Wl,--wrap=free', '-Wl,--wrap=sem_wait',
                   '-lcrypto', '-lm', '-o', str(cls.exe)]
        result = subprocess.run(command, capture_output=True, text=True, timeout=40)
        if result.returncode:
            cls.temp.cleanup()
            raise AssertionError(result.stdout + result.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def scenario(self, number):
        result = subprocess.run([str(self.exe), str(number)], capture_output=True,
                                text=True, timeout=12)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        print(result.stdout.strip())

    def test_body_allocation_failure_waits_before_execution(self): self.scenario(0)
    def test_receipt_allocation_failure_waits_before_execution(self): self.scenario(1)
    def test_results_maximum_id_and_post_start_heap_exhaustion(self): self.scenario(2)
    def test_eight_receipts_remain_bounded(self): self.scenario(3)
    def test_auth_revoked_while_waiting_at_real_gate(self): self.scenario(4)
    def test_core_wait_has_one_deadline_and_releases(self): self.scenario(5)
    def test_core_body_timeout_releases_credit(self): self.scenario(6)
    def test_core_disconnect_releases_credit(self): self.scenario(7)
    def test_core_fragmented_body_and_partial_response_sends(self): self.scenario(8)
    def test_receipts_interoperate_with_ordinary_dispatcher(self): self.scenario(9)
    def test_core_same_waiting_request_recovers_once(self): self.scenario(10)
    def test_live_heap_drop_between_claim_and_credit(self): self.scenario(11)
    def test_invalid_id_trailing_value_and_empty_body_do_not_execute(self): self.scenario(12)
    def test_failed_send_after_execution_retains_receipt_without_reexecution(self): self.scenario(13)
    def test_longest_legal_nonminimal_control_encodings_fit_input_bound(self): self.scenario(14)

    def test_every_control_envelope_fits_longest_legal_id(self):
        # Compile the actual serializer and vocabulary, rather than a second
        # codec. All legal identifier lengths and exact boundary capacities.
        from tests.test_runtime_mqtt_mp import function
        common = (ROOT / 'common/service_mp.inc').read_text()
        credit = (ROOT / 'common/service_http_control.inc').read_text()
        codes = re.search(r'static const char \*const codes\[\]=\{.*?\};', credit, re.S).group()
        fixture = '''#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "common/service_mp.h"
'''
        fixture += function(common, 'static const char *mp_message(') + '\n'
        fixture += function(credit, 'static size_t mp_control_reply(') + '\n'
        fixture += 'int main(void){' + codes + r'''
 char id[65],bytes[SERVICE_HTTP_CONTROL_REPLY];size_t maximum=0,minimum=SERVICE_HTTP_CONTROL_REPLY;
 for(unsigned n=0;n<sizeof(id);n++){
  memset(id,'x',n);id[n]=0;
  for(unsigned i=0;i<sizeof(codes)/sizeof(codes[0]);i++){
   size_t length=mp_control_reply(bytes,sizeof(bytes),id,codes[i]);
   assert(length&&length<=SERVICE_HTTP_CONTROL_REPLY);
   if(length>maximum)maximum=length;
   if(n&&length<minimum)minimum=length;
   assert(mp_control_reply(bytes,length+1,id,codes[i])==length);
   assert(!mp_control_reply(bytes,length,id,codes[i]));
   ph_value root,v;assert(phv_parse(PH_WIRE_MSGPACK,bytes,
       mp_control_reply(bytes,sizeof(bytes),id,codes[i]),&root));
   assert(phv_get(&root,"code",&v)&&phv_equal_text(&v,codes[i]));
   if(n)assert(phv_get(&root,"request_id",&v)&&phv_equal_text(&v,id));
  }
 }
 printf("all control envelopes: maximum=%zu capacity=%u id=0..64 PASS\n",maximum,SERVICE_HTTP_CONTROL_REPLY);
 printf("retained envelope wire range for id=1..64: %zu..%zu bytes\n",minimum,maximum);
}'''
        source = Path(self.temp.name) / 'envelope.c'
        source.write_text(fixture)
        exe = source.with_suffix('')
        result = subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11',
            '-DPHICOMM_STATUS_MODEL=2', '-fsanitize=address,undefined', '-no-pie',
            '-I'+str(ROOT), str(source), str(ROOT / 'common/typed_wire.c'),
            str(ROOT / 'common/wire_buffer.c'), '-lm', '-o', str(exe)],
            capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        print(result.stdout.strip())

    def test_ota_refusal_does_not_create_a_replay_tombstone(self):
        # The existing OTA dispatcher harness compiles the production include
        # with OTA enabled; ordinary scenarios link complete real service.c.
        source = Path(self.temp.name) / 'ota.c'
        fixture = (ROOT / 'tests/service_mp/fixture.inc').read_text()
        fixture += r'''
bool ph_auth_context_valid(const ph_auth_context *context,unsigned scopes){
 return context&&context->source==PH_AUTH_HTTP_OPEN&&(context->scopes&scopes)==scopes;
}
extern bool test_ota_active;
int main(void){
 service_init();ph_buf *q=request("indicator.set","ota-not-started",-1);
 void *credit=service_http_control_credit();assert(credit);
 char bytes[SERVICE_HTTP_CONTROL_REPLY];size_t length;unsigned status;
 ph_auth_context context={.source=PH_AUTH_HTTP_OPEN,.scopes=PH_AUTH_SCOPE_ADMIN};
 test_ota_active=true;allocation_budget=0;size_t allocations=total_allocations;
 assert(service_http_control_mp(q->buf,q->len,&context,&credit,bytes,sizeof(bytes),&length,&status));
 ph_buf response={.buf=bytes,.len=length};code(&response,"busy");
 assert(status==200&&credit&&!mutations&&total_allocations==allocations);
 /* The same id must be dispatchable after OTA; an erroneous receipt of the
  * NOT_STARTED busy would prevent this real unsupported-product result. */
 test_ota_active=false;
 assert(service_http_control_mp(q->buf,q->len,&context,&credit,bytes,sizeof(bytes),&length,&status));
 response.len=length;code(&response,"unsupported");assert(!credit&&!mutations&&total_allocations==allocations);
 ph_buf_free(q);puts("OTA NOT_STARTED busy: prepared credit retained, no receipt tombstone PASS");
}'''
        source.write_text(fixture)
        exe = source.with_suffix('')
        sources = [source, ROOT / 'tests/service_mp/ota_dispatcher.c',
                   ROOT / 'tests/service_mp/hooks.c', ROOT / 'common/typed_wire.c',
                   ROOT / 'common/wire_buffer.c']
        result = subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11',
            '-DMOC=1', '-DPHICOMM_DEVICE_OTA=1', '-DPHICOMM_STATUS_MODEL=2',
            '-Wno-deprecated-declarations', '-fsanitize=address,undefined', '-no-pie',
            '-I'+str(ROOT / 'tests/service_mp'), '-I'+str(ROOT), '-pthread',
            *map(str, sources), '-Wl,--wrap=malloc', '-Wl,--wrap=free',
            '-lcrypto', '-lm', '-o', str(exe)], capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        print(result.stdout.strip())

if __name__ == '__main__': unittest.main()
