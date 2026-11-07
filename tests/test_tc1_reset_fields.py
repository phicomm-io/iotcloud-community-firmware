"""TC1 boot diagnostics: reset cause mapping and the fields that report it."""
import pathlib, subprocess, tempfile, os, unittest

from tests.test_tc1_sdk_migration import function

ROOT = pathlib.Path(__file__).resolve().parents[1]


def execute(source):
    with tempfile.TemporaryDirectory() as d:
        p = pathlib.Path(d)
        (p / 'test.c').write_text(source)
        subprocess.run(['cc', '-std=c99', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                        str(p / 'test.c'), '-o', str(p / 'test')], check=True)
        subprocess.run([str(p / 'test')], check=True, timeout=8,
                       env={**os.environ, 'UBSAN_OPTIONS': 'halt_on_error=1'})


class TC1ResetFields(unittest.TestCase):
    def test_mc200_boot_flags_map_to_single_cause(self):
        reason = function((ROOT / 'TC1/main.c').read_text(), 'const char *tc1_reset_reason_text(')
        execute(r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
''' + reason + r'''
int main(void) {
    /* 0x08 is what an actual TC1 reported after losing its whole config. */
    assert(!strcmp(tc1_reset_reason_text(0x08), "software"));
    assert(!strcmp(tc1_reset_reason_text(0x20), "watchdog"));
    assert(!strcmp(tc1_reset_reason_text(0x10), "lockup"));
    assert(!strcmp(tc1_reset_reason_text(0x04), "power_av18"));
    assert(!strcmp(tc1_reset_reason_text(0x02), "power_av12"));
    assert(!strcmp(tc1_reset_reason_text(0x01), "power_vbat"));
    assert(!strcmp(tc1_reset_reason_text(0x00), "none"));
    /* Watchdog wins over any simultaneous power flag, as on A1/M1. */
    assert(!strcmp(tc1_reset_reason_text(0x21), "watchdog"));
    return 0;
}
''')

    def test_boot_flags_are_captured_before_defaults_can_run(self):
        main = (ROOT / 'TC1/main.c').read_text()
        # The flag has to be set before the application's own default branch,
        # otherwise a boot-time restore looks like an application decision.
        self.assertIn('if (!config_load_done) defaults_during_load = true;', main)
        self.assertIn('config_load_done=true;', main)
        self.assertLess(main.index('config_load_done=true;'), main.index('version_mismatch_reset=true;'))
        self.assertIn('if(defaults_during_load)tc1_log(', main)
        self.assertIn('reset reason raw[0x%x] %s', main)

    def test_diagnostics_are_exposed_as_optional_fields(self):
        httpd = (ROOT / 'TC1/http_server/app_httpd.c').read_text()
        for field in ('last_reset_reason_text', 'boot_defaults_applied',
                      'boot_version_mismatch', 'button_edges_without_gesture'):
            self.assertIn('"%s"' % field, httpd)
        header = (ROOT / 'TC1/main.h').read_text()
        for symbol in ('tc1_config_diag_t', 'tc1_config_diag(', 'tc1_reset_reason_text(',
                       'app_easylink_button_event('):
            self.assertIn(symbol, header)


if __name__ == '__main__':
    unittest.main()
