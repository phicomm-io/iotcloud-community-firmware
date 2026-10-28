"""SDK-independent MP business regression, including the full old scheduler matrix."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest
from tests.mp_fixture_vectors import header
ROOT = Path(__file__).resolve().parents[1]


class NativeFeatures(unittest.TestCase):
    def compile_run(self, source, defines=(), extra=(), vectors=None, controls=None):
        with tempfile.TemporaryDirectory(prefix='native-features-') as td:
            path = Path(td)
            if vectors is not None:
                (path / 'mp_fixture_vectors.inc').write_text(vectors)
            if controls is not None:
                (path / 'runtime_controls.inc').write_text(controls)
            cmd = shlex.split(os.environ.get('CC', 'cc'))
            cmd += ['-std=gnu99', '-Wall', '-Wextra', '-Werror', '-Wno-misleading-indentation', '-Wno-unused-function', '-Wno-sign-compare', '-g', '-I'+str(ROOT / 'tests/stubs'), '-I'+str(ROOT / 'common'), '-I'+str(path)]
            if os.environ.get('SANITIZE', '0') == '1':
                cmd += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie']
            cmd += ['-D'+d for d in defines]
            cmd += [str(ROOT / source), str(ROOT / 'common/typed_wire.c'), str(ROOT / 'common/wire_buffer.c')]
            cmd += [str(ROOT / p) for p in extra]
            cmd += ['-lm', '-o', str(path / 'probe')]
            built = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
            self.assertEqual(built.returncode, 0, built.stdout+built.stderr)
            ran = subprocess.run([str(path / 'probe')], capture_output=True, text=True, timeout=60)
            self.assertEqual(ran.returncode, 0, ran.stdout+ran.stderr)

    def test_config_all_six_domains(self):
        self.compile_run('tests/test_config_mp.c')

    def test_feature_business_cas_rollback_reads(self):
        self.compile_run('tests/test_feature_mp.c')

    def test_fresh_air_config_mp(self):
        self.compile_run('tests/test_fresh_air_mp.c')

    def test_automation_existing_scheduler_and_button_matrix(self):
        sources = [(ROOT / p).read_text() for p in ('tests/test_automation.c', 'tests/countdown_scenarios.inc')]
        for model, defines, macros in (
            ('A1', ['FEATURE_DEVICE_A1'], dict(CONTROL='fan.set', CONTROL_PARAMS='"on":true,"speed":50', KEY='1')),
            ('M1', [], dict(CONTROL='display.set', CONTROL_PARAMS='"brightness":50', KEY='0')),
            ('TC1', ['AUTOMATION_TC1'], dict(CONTROL='outlet.set', CONTROL_PARAMS='"on":true', KEY='0')),
        ):
            with self.subTest(model=model):
                self.compile_run('tests/test_automation.c', defines, vectors=header(sources, macros))

    def test_runtime_applied_state_profiles_animation_and_validation(self):
        from tests.test_runtime_mqtt_mp import function
        runtime = (ROOT / 'common/runtime.c').read_text()
        peer_start = runtime.index('typedef struct {', runtime.index('static unsigned available;'))
        peer_end = runtime.index('static peer_runtime_t peers[FEATURE_MAX_PEERS];', peer_start) + len('static peer_runtime_t peers[FEATURE_MAX_PEERS];')
        controls = runtime[peer_start:peer_end] + '\n' + function(runtime, 'static bool install_config(') + '\n'
        controls += '#include "feature_mp.inc"\n'
        controls += runtime[runtime.index('static void reset_controller(bool preserve_gate) {'):runtime.index('unsigned feature_mqtt_filters(')]
        sources = [(ROOT / p).read_text() for p in ('tests/test_runtime_controls_mp.c', 'tests/runtime_control_scenarios.inc')]
        self.compile_run('tests/test_runtime_controls_mp.c', vectors=header(sources, {}), controls=controls)

    def test_runtime_metrics_missing_heap_reset_cache_and_wrap(self):
        for model,defines in (('portable', []), ('moc-mx1290', ['MOC=1', 'CONFIG_CPU_MX1290'])):
            with self.subTest(model=model):
                self.compile_run('tests/test_runtime_metrics.c',defines)

    def test_engine_numeric_control_schedule_render_and_ids(self):
        self.compile_run('tests/test_engine.c', ['FEATURE_HOST'], ['common/engine.c'])

    def test_sensor_aggregation_partial_fields_freshness_co2(self):
        self.compile_run('tests/test_sensor_aggregate.c', ['FEATURE_HOST'], ['common/engine.c'])


if __name__ == '__main__':
    unittest.main()
