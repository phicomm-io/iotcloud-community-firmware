"""Release allocation boundaries are separate from the 12 KiB stress target."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from tests.test_http_json_budget import function
from tools.check_emw3080_memory import assess, PRESSURE_MINIMUM_BYTES


ROOT = Path(__file__).resolve().parents[1]
POLICY = 'PHICOMM_EMW3080_RELEASE_MEMORY=1'
VARIANTS = (
    ('M1', 'full', 1, 0),
    ('M1', 'no-frontend-resources', 0, 0),
    ('A1', 'full', 1, 0),
    ('A1', 'no-frontend-resources', 0, 0),
    ('A1', 'fresh-air', 1, 1),
    ('A1', 'fresh-air-no-frontend-resources', 0, 1),
)


def product_defines(product, build_type, resources, fresh_air):
    makefile = f'''BUILD_TYPE := {build_type}
PHICOMM_FULL_RESOURCES := {resources}
A1_FRESH_AIR := {fresh_air}
include {ROOT / product / (product + '.mk')}
.PHONY: flags
flags:
\t$(info $(GLOBAL_DEFINES))
'''
    return subprocess.check_output(
        ['make', '--no-print-directory', '-s', '-f', '-', 'flags'],
        input=makefile, text=True, cwd=ROOT).split()


class Emw3080ReleaseMemory(unittest.TestCase):
    def run_c(self, source, flags=()):
        with tempfile.TemporaryDirectory() as directory:
            probe = Path(directory) / 'probe.c'
            probe.write_text(source)
            command = [os.environ.get('CC', 'cc'), '-std=c99', '-Wall', '-Wextra',
                       '-Werror', '-I'+str(ROOT), *flags, str(probe), '-o', str(probe.with_suffix(''))]
            if os.environ.get('SANITIZE') == '1':
                command += ['-g', '-fno-omit-frame-pointer', '-fsanitize=address,undefined']
            subprocess.run(command, check=True)
            subprocess.run([str(probe.with_suffix(''))], check=True)

    def test_product_build_type_preprocessing(self):
        for product, variant, resources, fresh_air in VARIANTS:
            for build_type in ('release', 'release_log', 'debug'):
                with self.subTest(product=product, variant=variant, build_type=build_type):
                    defines = product_defines(product, build_type, resources, fresh_air)
                    release = build_type != 'debug'
                    self.assertEqual(POLICY in defines, release)
                    flags = ['-D'+POLICY] if release else []
                    # Actual pinned SDK flags: release_log and debug both use DEBUG.
                    flags += ['-DNDEBUG'] if build_type == 'release' else ['-DDEBUG']
                    source = '#include "common/memory_budget.h"\nSERVICE_MEMORY_RESERVE\n'
                    preprocessed = subprocess.check_output(
                        [os.environ.get('CC', 'cc'), '-E', '-P', '-x', 'c',
                         '-I'+str(ROOT), *flags, '-'], input=source, text=True)
                    self.assertEqual(preprocessed.splitlines()[-1], '8192u' if release else '16384u')
        for flags in ([], ['-DDEBUG'], ['-DNDEBUG'], ['-DPHICOMM_EMW3080_RELEASE_MEMORY=0']):
            with self.subTest(default_flags=flags):
                self.run_c('#include "common/memory_budget.h"\n'
                           '#if SERVICE_MEMORY_RESERVE != 16384u\n#error default changed\n#endif\n'
                           'int main(void){return 0;}\n', flags)
        for build_type in ('release', 'release_log', 'debug'):
            self.assertNotIn(POLICY, product_defines('TC1', build_type, 1, 0))

    def test_runtime_allocation_boundary_scenarios(self):
        service = (ROOT / 'common/service.c').read_text()
        mqtt = (ROOT / 'common/mqtt/client.c').read_text()
        production = '\n'.join((function(service, 'static uint32_t service_free_memory('),
                                function(service, 'void *ph_wire_malloc('),
                                function(mqtt, 'static bool permit(')))
        source = r'''
#undef NDEBUG
#include <assert.h>
#include <stdlib.h>
#include "common/memory_budget.h"
#define MOC 1
typedef struct { int free_memory; } micoMemInfo_t;
static micoMemInfo_t heap;
static bool memory_available=true;
static const micoMemInfo_t *MicoGetMemoryInfo(void){return memory_available?&heap:NULL;}
static bool wire_diag_ready;static uint32_t wire_alloc_failed,wire_guard_denied;static unsigned wire_diag_lock;
static void mico_rtos_lock_mutex(unsigned*p){(void)p;}static void mico_rtos_unlock_mutex(unsigned*p){(void)p;}
PRODUCTION
static bool native_permit(size_t n){void*p=ph_wire_malloc(n);bool yes=p!=NULL;free(p);return yes;}
int main(void){
 const uint32_t floor=SERVICE_MEMORY_RESERVE;
 service_memory_state_t state={.minimum_free=UINT32_MAX};
 assert(!memory_budget_claim(&state,floor-1,0));
 assert(!memory_budget_claim(&state,floor+511,0));
 unsigned first=memory_budget_claim(&state,floor+512,0);assert(first==512);
 assert(!memory_budget_claim(&state,floor+1023,0));
 unsigned second=memory_budget_claim(&state,floor+1024,0);assert(second==512);
 assert(state.active==2 && state.reserved==1024);
 memory_budget_release(&state,first);memory_budget_release(&state,second);
 assert(!state.active && !state.reserved);
 assert(!memory_budget_claim(&state,UINT32_MAX,SIZE_MAX));
 assert(!memory_budget_claim(&state,UINT32_MAX,16385));
 state.reserved=UINT32_MAX;assert(!memory_budget_claim(&state,UINT32_MAX,0));
 state.reserved=0;state.denied=UINT32_MAX;
 assert(!memory_budget_claim(&state,0,0) && state.denied==UINT32_MAX);
 unsigned maximum=memory_budget_claim(&state,floor+16896,16384);assert(maximum==16896);
 memory_budget_release(&state,maximum);assert(!state.active && !state.reserved);
 heap.free_memory=(int)floor-1;
 assert(!native_permit(0) && !permit(NULL,0));
 heap.free_memory=(int)floor+32;
 assert(!native_permit(1) && !permit(NULL,1));
 heap.free_memory=INT32_MAX;
 assert(!native_permit(SIZE_MAX) && !permit(NULL,SIZE_MAX));
 memory_available=false;assert(!native_permit(0) && !permit(NULL,0));memory_available=true;
 /* Boundary pressure is deliberately below the separate 12 KiB stress target.
  * Production binary/MQTT hooks must permit real mallocs while retaining 8 KiB. */
 if(floor==8192){
  for(uint32_t available=floor+33;available<12288;available+=127){
   heap.free_memory=(int)available;size_t bytes=available-floor-32;
   assert(native_permit(bytes) && permit(NULL,bytes));
   assert(!native_permit(bytes+1) && !permit(NULL,bytes+1));
   void *allocation=malloc(bytes);assert(allocation);
   heap.free_memory-=(int)(bytes+32);assert(heap.free_memory==(int)floor);
   assert(!native_permit(1) && !permit(NULL,1));free(allocation);
   state=(service_memory_state_t){.minimum_free=UINT32_MAX};
   if(available>=floor+512){
    unsigned claim=memory_budget_claim(&state,available,0);assert(claim==512);
    assert(available-state.reserved>=floor);memory_budget_release(&state,claim);
   }
  }
 }
 return 0;
}
'''.replace('PRODUCTION', production)
        for flags in ([], ['-DDEBUG'], ['-D'+POLICY, '-DDEBUG'], ['-D'+POLICY, '-DNDEBUG']):
            with self.subTest(flags=flags):
                self.run_c(source, flags)

    def test_stress_acceptance_keeps_12k_and_failed_values(self):
        self.assertEqual(PRESSURE_MINIMUM_BYTES, 12288)
        def sample(minimum, **extra):
            return {'code': 'ok', 'data': {'memory_reserve': 8192,
                    'free_memory': 20000, 'minimum_sampled_free': minimum, **extra}}
        passing = assess([sample(12288), sample(13000)])
        self.assertTrue(passing['passed'])
        self.assertEqual(passing['minimum_sampled_free'], 12288)
        for minimum in (8191, 8192, 10000, 12287):
            with self.subTest(minimum=minimum):
                result = assess([sample(minimum, ssid='private', mqtt_password='private')])
                self.assertFalse(result['passed'])
                self.assertEqual(result['minimum_sampled_free'], minimum)
                self.assertEqual(result['samples'][0]['metrics']['minimum_sampled_free'], minimum)
                self.assertNotIn('private', str(result))
        self.assertFalse(assess([sample(13000, memory_reserve=16384)])['passed'])
        self.assertFalse(assess([sample(13000, free_memory=10000)])['passed'])
        self.assertFalse(assess([{'memory_reserve': 8192, 'free_memory': 20000}])['passed'])
        self.assertFalse(assess([{'code': 'unavailable'}])['passed'])
        self.assertFalse(assess([])['passed'])
        for key in ('minimum_free', 'min_free_memory', 'min_observed_free_memory'):
            self.assertFalse(assess([{'memory_reserve': 8192, key: 12287}])['passed'])
            self.assertTrue(assess([{'memory_reserve': 8192, key: 12288}])['passed'])
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'samples.json'
            for minimum, status in ((12288, 0), (10000, 1)):
                path.write_text(json.dumps([sample(minimum, ssid='private')]))
                result = subprocess.run([sys.executable, str(ROOT / 'tools/check_emw3080_memory.py'),
                                         str(path)], capture_output=True, text=True)
                self.assertEqual(result.returncode, status)
                self.assertEqual(json.loads(result.stdout)['minimum_sampled_free'], minimum)
                self.assertNotIn('private', result.stdout)

    def test_invalid_present_metrics_fail_without_hiding_negative_values(self):
        valid = {'memory_reserve': 8192, 'free_memory': 20000,
                 'minimum_sampled_free': 13000}
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'samples.json'
            for field in ('memory_reserve', 'free_memory', 'minimum_sampled_free', 'minimum_free'):
                for value in (-1, False, True, 13000.0, -1.5, float('nan'), 'private', None, [], {}):
                    with self.subTest(field=field, value=value):
                        sample = {**valid, field: value, 'ssid': 'private'}
                        report = assess([sample])
                        self.assertFalse(report['passed'])
                        self.assertIn('invalid_metric_'+field, report['samples'][0]['failures'])
                        diagnostic = report['samples'][0]['invalid_metrics'][field]
                        self.assertEqual(diagnostic['type'], type(value).__name__)
                        if type(value) is int:
                            self.assertEqual(report['samples'][0]['metrics'][field], value)
                        self.assertNotIn('private', str(report))
                        path.write_text(json.dumps([sample]))
                        result = subprocess.run([sys.executable, str(ROOT / 'tools/check_emw3080_memory.py'),
                                                 str(path)], capture_output=True, text=True)
                        self.assertEqual(result.returncode, 1)
                        saved = json.loads(result.stdout)
                        self.assertFalse(saved['passed'])
                        self.assertEqual(saved['samples'][0]['invalid_metrics'][field]['type'],
                                         type(value).__name__)
                        if type(value) is int:
                            self.assertEqual(saved['samples'][0]['metrics'][field], value)
                        self.assertNotIn('private', result.stdout)


if __name__ == '__main__':
    unittest.main()
