"""Run M1 reset/UART/boot code against the pinned SDK's actual Flash routines."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from tests.host_models import selected_models
from tests.test_config_commit import CHECKSUM, ROOT, STORAGE, apply, function, pinned_bytes, sdk_for


class M1ConfigRecoveryTest(unittest.TestCase):
    def test_reset_receipts_persistence_failure_and_cold_boot(self):
        sdk = sdk_for('M1')
        if 'M1' not in selected_models() or not (sdk / '.git').exists():
            self.skipTest('pinned M1 SDK checkout unavailable or not selected')
        main = (ROOT / 'M1/main.c').read_text()
        uart = (ROOT / 'M1/uart.c').read_text()
        engine = (ROOT / 'common/engine.c').read_text()
        prelude = ('#define mico_system_context_update host_inline_update\n'
                   '#include "M1/storage.h"\n#undef mico_system_context_update\n')
        includes = ['-I' + str(p) for p in (ROOT, ROOT / 'M1', ROOT / 'tests/host',
                    sdk / 'libraries/utilities/json_c')]
        with tempfile.TemporaryDirectory(prefix='m1-recovery-sdk-') as temporary:
            temporary = Path(temporary)
            # Derive the size from production headers instead of a stale fixture.
            size_probe, size_binary = temporary / 'size.c', temporary / 'size'
            size_probe.write_text(prelude + 'int main(void){printf("%zu",sizeof(app_storage_t));}\n')
            subprocess.run([os.environ.get('CC', 'cc'), *includes, str(size_probe), '-o', str(size_binary)], check=True)
            app_size = subprocess.check_output([str(size_binary)], text=True)
            target = temporary / STORAGE
            target.parent.mkdir(parents=True)
            target.write_bytes(pinned_bytes(sdk, STORAGE))
            apply(temporary)
            source = target.read_text()
            checksum = pinned_bytes(sdk, CHECKSUM).decode().replace('\r\n', '\n')
            actual = checksum[checksum.index('uint16_t UpdateCRC16'):]
            actual += source[source.index('static bool para_commit_uncertain'):source.index('OSStatus mico_system_context_restore')]
            actual += function(source, 'OSStatus MICOReadConfiguration(')
            actual += function(source, 'OSStatus mico_system_context_update(')
            fixture = (ROOT / 'tests/config_commit.c').read_text()
            fixture = fixture.replace('APP_SIZE = 2928', 'APP_SIZE = ' + app_size)
            fixture = fixture.replace('bool app_config_persist_allowed(void) { return persist_allowed; }',
                                      'extern bool app_config_persist_allowed(void);')
            fixture = fixture.replace('void app_config_commit_indeterminate(void) { indeterminate_hooks++; }',
                                      'extern void app_config_commit_indeterminate(void);')
            fixture = fixture.replace('/* SDK_FUNCTIONS */', actual).replace('int main(void)', 'int original_fixture_main(void)')
            fixture += r'''
void sdk_prepare(void) { setup(); }
void *sdk_application(void) { return app; }
void *sdk_context(void) { return &context.flashContentInRam; }
int sdk_update(void *p) { return mico_system_context_update(p); }
unsigned sdk_mutations(void) { return mutations; }
void sdk_fail_next_read(void) { reset_io(); fault_at=1; fault_mode=BEFORE; }
void sdk_clear_faults(void) { reset_io(); }
void sdk_latch_uncertain(void) { para_commit_uncertain=true; app_config_commit_indeterminate(); }
int sdk_cold_read(void) {
    para_commit_uncertain=false; reset_io(); memset(app,0,sizeof(app));
    memset(&context.flashContentInRam,0,sizeof(context.flashContentInRam));
    return MICOReadConfiguration(&context);
}
'''
            sdk_probe = temporary / 'sdk.c'
            sdk_probe.write_text(fixture)
            product = prelude + main[main.index('static bool config_recovery'):main.index('#include "../common/service.h"')]
            product += function(engine, 'void feature_co2_defaults(')
            product += function(engine, 'void feature_defaults(')
            product += function(engine, 'void feature_light_layout(')
            product += function((ROOT / 'common/automation.c').read_text(), 'void automation_defaults(')
            product += r'''
static mico_mutex_t config_lock;
mico_mutex_t m1_lock;
static m1_config_t *app_config;
m1_state_t m1_state;
mico_Context_t *m1_context;
void sdk_prepare(void); void *sdk_application(void); void *sdk_context(void);
int sdk_update(void *); unsigned sdk_mutations(void); void sdk_fail_next_read(void);
void sdk_clear_faults(void); void sdk_latch_uncertain(void); int sdk_cold_read(void);
#define mico_system_context_update sdk_update
uint32_t mico_rtos_get_time(void) { return 12345; }
void service_event(const char *level,const char *domain,const char *event) {
    assert(!strcmp(level,"error") && !strcmp(domain,"config") && !strcmp(event,"unsupported_layout"));
}
'''
            product += function(main, 'void appRestoreDefault_callback(')
            product += '#define AUTH_STORE (((app_storage_t *)app_config)->local_auth)\n'
            product += 'ph_auth_state *device_local_auth(void) { return NULL; }\n'
            product += main[main.index('typedef struct {const m1_config_t *config;'):main.index('void m1_co2_config_get(')]
            product += function(main, 'static bool m1_reset_commit(')
            product += function(main, 'bool m1_reset(')
            gate = main.index('    if (app_config->magic != M1_CONFIG_MAGIC')
            product += 'static void boot_gate(void) {\n' + main[gate:main.index('    bool migration_changed', gate)] + '}\n'
            product += (ROOT / 'tests/m1_config_recovery.c').read_text().replace('/* RESET_FRAME */',
                uart[uart.index('typedef struct { bool committed;'):uart.index('static bool reset_frame(')] + function(uart, 'static bool reset_frame('))
            probe, binary = temporary / 'product.c', temporary / 'probe'
            probe.write_text(product)
            subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu99', '-O1', '-g', '-fno-omit-frame-pointer',
                            '-fsanitize=address,undefined', *includes, str(probe), str(sdk_probe), str(ROOT / 'common/local_auth.c'), '-o', str(binary)], check=True)
            environment = os.environ.copy()
            environment.update(ASAN_OPTIONS='detect_leaks=1:abort_on_error=1', UBSAN_OPTIONS='halt_on_error=1')
            subprocess.run([str(binary)], env=environment, check=True)
