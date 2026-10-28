"""Fault the actual pinned SDK commit/boot functions, never shared SDK caches."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from tests.host_models import selected_models

ROOT = Path(__file__).resolve().parents[1]
STORAGE = 'MiCO/system/mico_system_para_storage.c'
CHECKSUM = 'libraries/utilities/CheckSumUtils.c'
APPLY = ROOT / 'tools/apply_sdk_patch.py'
PATCHES = [ROOT / 'common' / name for name in (
    'config-recovery-mico36.patch', 'config-commit-mico36.patch',
    'config-write-bounded-mico36.patch')]
SDK_NAMES = {'M1': 'm1-sdk', 'A1': 'a1-sdk', 'TC1': 'tc1-sdk-3.6.4'}


def function(source, signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'


def sdk_for(model):
    return Path(os.environ.get(model + '_SDK_DIR', ROOT / 'build' / SDK_NAMES[model] / 'mico-os'))


def pinned_bytes(sdk, path):
    return subprocess.check_output(['git', '-C', str(sdk), 'show', 'HEAD:' + path])


def apply(sdk, patches=PATCHES, check=True):
    return subprocess.run(['python3', str(APPLY), str(sdk), *map(str, patches)],
                          check=check, capture_output=True, text=True)


class ConfigCommitTests(unittest.TestCase):
    def test_ordered_overlay_from_pristine_old_cache_and_product_overlays(self):
        seen = 0
        pin = json.loads((ROOT / 'common/mico-sdk.json').read_text())['commit']
        for model in selected_models():
            sdk = sdk_for(model)
            if not (sdk / '.git').exists():
                continue
            seen += 1
            self.assertEqual(subprocess.check_output(['git', '-C', str(sdk), 'rev-parse', 'HEAD'], text=True).strip(), pin)
            baseline = pinned_bytes(sdk, STORAGE)
            for prefix in range(len(PATCHES) + 1):
                with self.subTest(model=model, prefix=prefix), tempfile.TemporaryDirectory(prefix='config-commit-stack-') as tmp:
                    isolated = Path(tmp)
                    target = isolated / STORAGE
                    target.parent.mkdir(parents=True)
                    target.write_bytes(baseline)
                    for patch in PATCHES[:prefix]:
                        apply(isolated, [patch])
                    target.write_bytes(target.read_bytes() + b'\n/* unrelated local note */\n')
                    apply(isolated)
                    complete = target.read_bytes()
                    apply(isolated)
                    self.assertEqual(complete, target.read_bytes())
                    self.assertIn(b'unrelated local note', target.read_bytes())
                    # Reject a changed commit function without any partial write.
                    target.write_bytes(complete.replace(b'if (!inContext || para_commit_uncertain) return kNotPreparedErr;', b'if (!inContext || para_commit_uncertain) return -123;'))
                    changed = target.read_bytes()
                    result = apply(isolated, check=False)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn('SDK left unchanged', result.stderr)
                    self.assertEqual(changed, target.read_bytes())
            # Existing product caches contain recovery + their unrelated board,
            # OTA and TC1 old-layout overlays. Upgrade only a disposable copy.
            with self.subTest(model=model, cache='existing'), tempfile.TemporaryDirectory(prefix='config-commit-cache-') as tmp:
                target = Path(tmp) / STORAGE
                target.parent.mkdir(parents=True)
                current = (sdk / STORAGE).read_bytes()
                target.write_bytes(current)
                apply(Path(tmp))
                complete = target.read_bytes()
                apply(Path(tmp))
                self.assertEqual(complete, target.read_bytes())
                self.assertEqual(current, (sdk / STORAGE).read_bytes())
        if not seen:
            self.skipTest('pinned product SDK checkout unavailable')

    def test_actual_sdk_functions_flash_matrix_and_powerloss(self):
        seen = 0
        fixture = (ROOT / 'tests/config_commit.c').read_text()
        for model in selected_models():
            sdk = sdk_for(model)
            if not (sdk / '.git').exists():
                continue
            seen += 1
            with self.subTest(model=model), tempfile.TemporaryDirectory(prefix='config-commit-sdk-') as tmp:
                temporary = Path(tmp)
                target = temporary / STORAGE
                target.parent.mkdir(parents=True)
                target.write_bytes(pinned_bytes(sdk, STORAGE))
                apply(temporary)
                source = target.read_text()
                checksum = pinned_bytes(sdk, CHECKSUM).decode().replace('\r\n', '\n')
                core = source[source.index('static bool para_commit_uncertain'):source.index('OSStatus mico_system_context_restore')]
                extracted = checksum[checksum.index('uint16_t UpdateCRC16'):]
                extracted += core
                extracted += function(source, 'OSStatus MICOReadConfiguration(')
                extracted += function(source, 'OSStatus mico_system_context_update(')
                probe = temporary / 'probe.c'
                probe.write_text(fixture.replace('/* SDK_FUNCTIONS */', extracted))
                for extra_ap in (False, True):
                    binary = temporary / ('probe-extra-ap' if extra_ap else 'probe')
                    command = [os.environ.get('CC', 'cc'), '-std=gnu99', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                               '-fno-omit-frame-pointer', '-fsanitize=address,undefined', str(probe), '-o', str(binary)]
                    if extra_ap:
                        command += ['-DMICO_EXTRA_AP_NUM=2']
                    subprocess.run(command, check=True)
                    environment = os.environ.copy()
                    environment['ASAN_OPTIONS'] = 'detect_leaks=1:abort_on_error=1'
                    environment['UBSAN_OPTIONS'] = 'halt_on_error=1'
                    result = subprocess.run([str(binary)], env=environment, check=True, capture_output=True, text=True)
                    print(model, 'extra_ap=' + str(extra_ap), 'SDK_source_sha256=' + hashlib.sha256(target.read_bytes()).hexdigest(), flush=True)
                    print(result.stdout, end='', flush=True)
                    # The injected power loss intentionally abandons SDK heap
                    # objects; process exit releases them, as a real reset does.
                    environment['CONFIG_COMMIT_POWERLOSS'] = '1'
                    environment['ASAN_OPTIONS'] = 'detect_leaks=0:abort_on_error=1'
                    result = subprocess.run([str(binary)], env=environment, check=True, capture_output=True, text=True)
                    self.assertIn('power loss:', result.stdout)
                    print(result.stdout, end='', flush=True)
        if not seen:
            self.skipTest('pinned product SDK checkout unavailable')

    def test_previous_overlay_fails_hidden_kernel_allocation_contract(self):
        sdk = next((sdk_for(model) for model in selected_models()
                    if (sdk_for(model) / '.git').exists()), None)
        if sdk is None:
            self.skipTest('pinned product SDK checkout unavailable')
        with tempfile.TemporaryDirectory(prefix='config-commit-old-write-') as tmp:
            temporary = Path(tmp)
            target = temporary / STORAGE
            target.parent.mkdir(parents=True)
            target.write_bytes(pinned_bytes(sdk, STORAGE))
            apply(temporary, PATCHES[:-1])
            source = target.read_text()
            checksum = pinned_bytes(sdk, CHECKSUM).decode().replace('\r\n', '\n')
            actual = checksum[checksum.index('uint16_t UpdateCRC16'):]
            actual += source[source.index('static bool para_commit_uncertain'):source.index('OSStatus mico_system_context_restore')]
            actual += function(source, 'OSStatus MICOReadConfiguration(')
            actual += function(source, 'OSStatus mico_system_context_update(')
            probe, binary = temporary / 'probe.c', temporary / 'probe'
            probe.write_text((ROOT / 'tests/config_commit.c').read_text().replace('/* SDK_FUNCTIONS */', actual))
            subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu99', '-O1', '-g',
                            '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                            str(probe), '-o', str(binary)], check=True)
            environment = os.environ.copy()
            environment['CONFIG_COMMIT_BOUNDED_WRITE_ONLY'] = '1'
            environment['ASAN_OPTIONS'] = 'detect_leaks=0:abort_on_error=1'
            result = subprocess.run([str(binary)], env=environment, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('mico_system_context_update(mico_system_context_get()) == kNoErr', result.stderr)
            print('previous actual SDK overlay: 4648-byte app fails the 1024-byte hidden allocation contract (negative control) PASS', flush=True)

    def test_actual_product_hooks_remain_readonly_until_restart(self):
        sdk = sdk_for('M1')
        if not (sdk / '.git').exists():
            self.skipTest('pinned product SDK checkout unavailable')
        with tempfile.TemporaryDirectory(prefix='config-commit-hooks-') as tmp:
            temporary = Path(tmp)
            target = temporary / STORAGE
            target.parent.mkdir(parents=True)
            target.write_bytes(pinned_bytes(sdk, STORAGE))
            apply(temporary)
            weak = function(target.read_text(), 'WEAK void app_config_commit_indeterminate(')
            weak = '#define WEAK __attribute__((weak))\n' + weak
            weak += 'void invoke_sdk_hook(void) { app_config_commit_indeterminate(); }\n'
            (temporary / 'weak.c').write_text(weak)
            for model in selected_models():
                source = (ROOT / model / 'main.c').read_text()
                start = source.index('static bool config_recovery')
                actual = source[start:source.index('#include', start)]
                driver = '#include <assert.h>\n#include <stdbool.h>\n' + actual
                driver += '''
void invoke_sdk_hook(void);
int main(void) {
 assert(!device_config_recovery() && app_config_persist_allowed());
 device_config_recovery_set(true);assert(device_config_recovery() && !app_config_persist_allowed());
 device_config_recovery_set(false);assert(!device_config_recovery() && app_config_persist_allowed());
 invoke_sdk_hook();assert(device_config_recovery() && !app_config_persist_allowed());
 device_config_recovery_set(false);assert(device_config_recovery() && !app_config_persist_allowed());
 config_recovery=false;assert(device_config_recovery() && !app_config_persist_allowed());
 return 0;
}
'''
                probe, binary = temporary / (model + '.c'), temporary / model
                probe.write_text(driver)
                subprocess.run([os.environ.get('CC', 'cc'), '-std=c99', '-Wall', '-Wextra', '-Werror',
                                '-g', '-fsanitize=address,undefined', str(probe), str(temporary / 'weak.c'),
                                '-o', str(binary)], check=True)
                subprocess.run([str(binary)], check=True)
                print(model, 'actual main hook + actual SDK weak override: recovery cannot be cleared this boot PASS', flush=True)

    def test_actual_public_wrappers_strong_hooks_and_trusted_reread(self):
        fixture = (ROOT / 'tests/config_commit.c').read_text()
        for model in selected_models():
            sdk = sdk_for(model)
            if not (sdk / '.git').exists():
                continue
            with self.subTest(model=model), tempfile.TemporaryDirectory(prefix='config-commit-public-') as tmp:
                temporary = Path(tmp)
                target = temporary / STORAGE
                target.parent.mkdir(parents=True)
                target.write_bytes(pinned_bytes(sdk, STORAGE))
                apply(temporary)
                source = target.read_text()
                product = (ROOT / model / 'main.c').read_text()
                start = product.index('static bool config_recovery')
                hooks = product[start:product.index('#include', start)]
                checksum = pinned_bytes(sdk, CHECKSUM).decode().replace('\r\n', '\n')
                extracted = checksum[checksum.index('uint16_t UpdateCRC16'):]
                extracted += source[source.index('static bool para_commit_uncertain'):source.index('OSStatus mico_system_context_restore')]
                extracted += function(source, 'OSStatus mico_system_context_restore(')
                extracted += function(source, 'OSStatus MICORestoreMFG(')
                extracted += function(source, 'OSStatus MICOReadConfiguration(')
                extracted += function(source, 'OSStatus mico_system_context_update(')
                probe_source = fixture.replace('int main(void)', 'int author_fixture_main(void)')
                # Compile the real initializer as well: it ignores the reader's
                # return code, so fresh flags must be latched by the reader.
                initializer = function(source, 'void* mico_system_context_init(')
                getter_stub = function(probe_source, 'static mico_Context_t *mico_system_context_get(')
                probe_source = probe_source.replace(getter_stub, function(source, 'mico_Context_t* mico_system_context_get('))
                probe_source = probe_source.replace('    system_config_t flashContentInRam;',
                    '    system_config_t flashContentInRam;\n    mico_mutex_t flashContentInRam_mutex;', 1)
                probe_source = probe_source.replace('#define para_log(...) ((void)0)',
                    '#define para_log(...) ((void)0)\n#define system_log(...) ((void)0)\n'
                    '#define require(c,l) do { if (!(c)) goto l; } while (0)')
                probe_source = probe_source.replace('static void mico_rtos_lock_mutex(',
                    'static void mico_rtos_init_mutex(mico_mutex_t *mutex) { *mutex = 0; }\n'
                    'static void mico_rtos_lock_mutex(', 1)
                probe_source = probe_source.replace('static mico_mutex_t para_flash_mutex;',
                    'static mico_mutex_t para_flash_mutex;\nstatic void (*lock_handoff)(void);')
                probe_source = probe_source.replace('assert(!*mutex); *mutex = 1;',
                    'assert(!*mutex); *mutex = 1; if (lock_handoff) lock_handoff();')
                probe_source = probe_source.replace('bool app_config_persist_allowed(void) { return persist_allowed; }', hooks)
                probe_source = probe_source.replace('void app_config_commit_indeterminate(void) { indeterminate_hooks++; }', '')
                # Only the surrounding SDK type/layout callback needs extra
                # fields for compiling its real RAM-default/MFG restore body.
                probe_source = probe_source.replace('    char localIp[16], netMask[16], gateWay[16], dnsServer[16];',
                    '    char name[32]; int configured, easyLinkByPass; bool rfPowerSaveEnable, mcuPowerSaveEnable;\n'
                    '    char localIp[16], netMask[16], gateWay[16], dnsServer[16];', 1)
                probe_source = probe_source.replace('#define DHCP_Disable 0',
                    '#define DHCP_Disable 0\n#define DEFAULT_NAME "Test"\n#define unConfigured 0\n'
                    '#define mfgConfigured 2\n#define EASYLINK_BYPASS_NO 0')
                restore_stub = function(probe_source, 'static OSStatus mico_system_context_restore(')
                probe_source = probe_source.replace(restore_stub,
                    'OSStatus mico_system_context_restore(mico_Context_t * const inContext);\n'
                    'void appRestoreDefault_callback(void *data, uint32_t size) { restore_calls++; memset(data,0,size); }\n')
                probe_source = probe_source.replace('/* SDK_FUNCTIONS */', extracted)
                init_check = ''
                if model == 'A1':
                    init_check = '''
 config_initializing=true;config_update_pending=false;
 assert(mico_system_context_update(mico_system_context_get())==kNoErr && config_update_pending && !operation_count);
 config_initializing=false;config_update_pending=false;
'''
                driver = r'''
static void latch_during_mutex_handoff(void) {
 lock_handoff=NULL;para_commit_uncertain=true;app_config_commit_indeterminate();
}
static void cold_flags(void) {
 sys_context=&context;config_recovery=false;config_commit_uncertain=false;para_commit_uncertain=false;
 COLD_A1
}
static void fresh_init_read_errors(void) {
 cold_flags();setup();assert(MICOReadConfiguration(&context)==kNoErr);
 unsigned selected[5]={1},sections=1;
 for(unsigned i=0;i<operation_count;i++) {
  operation_t op=operations[i];
  if(op.kind!='R' || op.part)continue;
  if(op.offset==FLASH_SIZE-CRC_SIZE && op.length==CRC_SIZE)selected[sections++]=i+1;
  if((op.offset==0 && op.length==sizeof(system_config_t)) ||
     (op.offset==sizeof(system_config_t) && op.length==APP_SIZE))selected[sections++]=i+1;
#ifdef MICO_EXTRA_AP_NUM
  if(op.offset==EXTRA_OFFSET && op.length==sizeof(context.extra_ap))selected[sections++]=i+1;
#endif
 }
 assert(sections==4
#ifdef MICO_EXTRA_AP_NUM
  +1
#endif
 );
 unsigned cases=0;
 for(unsigned section=0;section<sections;section++) for(unsigned m=0;m<3;m++) {
  cold_flags();setup();uint8_t before[2][FLASH_SIZE];memcpy(before,flash,sizeof(before));
  fault_at=selected[section];fault_mode=(unsigned[]){BEFORE,AFTER,SHORT}[m];
  sys_context=NULL; /* Actual cold init owns new context/user allocations. */
  assert(mico_system_context_init(APP_SIZE)!=NULL);
  assert(para_commit_uncertain && config_commit_uncertain && device_config_recovery() && !app_config_persist_allowed());
  assert(!mutations && !memcmp(before,flash,sizeof(before)) && !live_bytes);
  unsigned calls=operation_count;
  assert(mico_system_context_update(mico_system_context_get())==kNotPreparedErr && operation_count==calls);
  free(sys_context->user_config_data);free(sys_context);sys_context=&context;cases++;
 }
 cold_flags();
 printf("MODEL actual SDK init ignores reader error; %u fresh CRC/system/app/extra before/after/short errors latch SDK+product, later actual public save refuses with zero mutations PASS\n",cases);
}
int main(void) {
 setup();
 device_config_recovery_set(true);
 assert(!para_commit_uncertain && !app_config_persist_allowed());
 assert(mico_system_context_update(mico_system_context_get())==kNoErr && !operation_count);
 device_config_recovery_set(false);
 INIT_CHECK
 operation_t normal[MAX_OPS];unsigned n=normal_operations(normal),tail_read=0;
 for(unsigned i=1;i<n;i++)
  if(normal[i-1].kind=='W' && normal[i-1].part==0 && normal[i-1].offset==FLASH_SIZE-CRC_SIZE)tail_read=i+1;
 assert(tail_read);setup();set_revision(8);fault_at=tail_read;fault_mode=PERSISTENT;
 assert(mico_system_context_update(mico_system_context_get())==FAULT);
 assert(para_commit_uncertain && config_commit_uncertain && device_config_recovery() && !app_config_persist_allowed());
 set_revision(7); /* Actual caller's RAM rollback, without claiming Flash old. */
 unsigned calls=operation_count;
 assert(mico_system_context_update(mico_system_context_get())==kNotPreparedErr && operation_count==calls);
 system_config_t saved_system=context.flashContentInRam;
 uint8_t saved_app[APP_SIZE];memcpy(saved_app,app,sizeof(app));
 assert(mico_system_context_restore(mico_system_context_get())==kNotPreparedErr);
 assert(MICORestoreMFG()==kNotPreparedErr);
 assert(!memcmp(&saved_system,&context.flashContentInRam,sizeof(saved_system)) && !memcmp(saved_app,app,sizeof(app)));
 assert(operation_count==calls && !restore_calls);
 device_config_recovery_set(false);
 assert(device_config_recovery() && !app_config_persist_allowed());
 reset_io();
 assert(MICOReadConfiguration(&context)==kNoErr && revision()==8 && !mutations);
 assert(para_commit_uncertain && config_commit_uncertain && device_config_recovery());
 calls=operation_count;
 assert(mico_system_context_update(mico_system_context_get())==kNotPreparedErr);
 assert(mico_system_context_restore(mico_system_context_get())==kNotPreparedErr);
 assert(MICORestoreMFG()==kNotPreparedErr);
 assert(operation_count==calls && !restore_calls);
 /* Simulated cold boot clears RAM-only static flags, then performs the actual
  * complete SDK read before ordinary persistence is allowed again. */
 config_recovery=false;config_commit_uncertain=false;para_commit_uncertain=false;
 reset_io();assert(MICOReadConfiguration(&context)==kNoErr && revision()==8);
 assert(!device_config_recovery() && app_config_persist_allowed());
 set_revision(9);assert(mico_system_context_update(mico_system_context_get())==kNoErr && raw_valid(0));
 calls=operation_count;lock_handoff=latch_during_mutex_handoff;
 assert(mico_system_context_update(mico_system_context_get())==kNotPreparedErr);
 assert(operation_count==calls && para_commit_uncertain && config_commit_uncertain && !para_flash_mutex);
 puts("MODEL actual public update/RAM-restore/MFG-restore + actual strong hooks: indeterminate rejects, trusted reread remains locked this boot, cold boot restores; ordinary init/recovery merge preserved PASS");
 puts("MODEL actual SDK mutex handoff: newly latched concurrent uncertainty rejects queued write before Flash I/O PASS");
 fresh_init_read_errors();
 return 0;
}
'''.replace('INIT_CHECK', init_check).replace('COLD_A1',
    'config_initializing=false;config_update_pending=false;' if model == 'A1' else '').replace('MODEL', model)
                probe, binary = temporary / 'public.c', temporary / 'public'
                # The initializer allocates context/user blocks with calloc;
                # compile it outside the fixture's 1 KiB scratch allocator.
                probe.write_text(probe_source + initializer + driver)
                environment = os.environ.copy()
                environment['ASAN_OPTIONS'] = 'detect_leaks=1:abort_on_error=1'
                environment['UBSAN_OPTIONS'] = 'halt_on_error=1'
                for extra_ap in (False, True):
                    command = [os.environ.get('CC', 'cc'), '-std=gnu99', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                               '-fno-omit-frame-pointer', '-fsanitize=address,undefined', str(probe), '-o', str(binary)]
                    if extra_ap:
                        command += ['-DMICO_EXTRA_AP_NUM=2']
                    subprocess.run(command, check=True)
                    subprocess.run([str(binary)], env=environment, check=True)

    def test_tc1_old_layout_migration_with_new_commit_overlay(self):
        if 'TC1' not in selected_models():
            self.skipTest('TC1 excluded by PHICOMM_TEST_MODELS')
        sdk = sdk_for('TC1')
        if not (sdk / STORAGE).exists():
            self.skipTest('TC1 SDK checkout unavailable')
        with tempfile.TemporaryDirectory(prefix='config-commit-tc1-migration-') as tmp:
            temporary = Path(tmp)
            target = temporary / STORAGE
            target.parent.mkdir(parents=True)
            # The prepared TC1 copy already has its real legacy-layout reader.
            target.write_bytes((sdk / STORAGE).read_bytes())
            apply(temporary, [ROOT / 'TC1/parameters-sdk.patch'])
            apply(temporary)
            environment = os.environ.copy()
            environment['TC1_SDK_DIR'] = str(temporary)
            subprocess.run(['python3', '-m', 'unittest', 'tests.test_tc1_sdk_migration'],
                           cwd=ROOT, env=environment, check=True)

    def test_builds_use_ordered_recovery_then_commit_overlay(self):
        for model in selected_models():
            source = (ROOT / model / 'build.sh').read_text()
            before = source.index('common/config-recovery-mico36.patch')
            after = source.index('common/config-commit-mico36.patch')
            self.assertLess(before, after)
            self.assertIn('\\\n', source[before:after])
            self.assertEqual(source.count('common/config-commit-mico36.patch'), 1)
            bounded = source.index('common/config-write-bounded-mico36.patch')
            self.assertLess(after, bounded)
            self.assertIn('\\\n', source[after:bounded])
            self.assertEqual(source.count('common/config-write-bounded-mico36.patch'), 1)


if __name__ == '__main__':
    unittest.main()
