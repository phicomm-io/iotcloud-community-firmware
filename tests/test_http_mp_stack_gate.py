"""Negative fixtures for the strict linked MessagePack HTTP stack gate."""
from contextlib import redirect_stdout
import io
from pathlib import Path
import tempfile
import unittest
from tools.check_http_core import (HTTP_BUDGET, HTTP_RESERVE, check_symbols,
    http_paths, linked_symbols, read_frames, validate_paths, check_tc1_ota_decimal)
from tools.status_stack import status_paths

class HTTPMessagePackStackGate(unittest.TestCase):
    def fixture(self,model='A1'):
        # First discover required public/noinline frames. Emit all known
        # private helpers as a separate second pass, simulating un-inlining.
        paths=http_paths({},model)
        frames={name:16 for reserve,names in paths.values() for name in names}
        for name in ['mp_decode','phv_scan','phv_next','phv_duplicate','phv_head',
                     'phv_skip_validated','binary_token','phw_bytes','phw_prefix',
                     'phw_quote','phw_head','device_status_visit','emit_string',
                     'runtime_fields','mp_copy','memory_response','acquire',
                     'action','read_body','close_slot','respond','transmit']:
            frames[name]=16
        return frames,http_paths(frames,model,frames)
    def test_actual_mp_paths_and_unchanged_budget(self):
        for model in ['A1','M1','TC1']:
            frames,paths=self.fixture(model)
            with redirect_stdout(io.StringIO()):validate_paths(paths,frames)
            names={n for reserve,stack in paths.values() for n in stack}
            self.assertTrue({'mp_dispatch','mp_cached','service_product_status_mp',
                             'device_status_mp','phv_scan','phw_cached_value'}<=names)
            self.assertTrue(all(reserve>=3072 for reserve,names in paths.values()))
            self.assertFalse(any('json' in n for n in names))
            with self.assertRaisesRegex(ValueError,'budget must remain'):
                validate_paths(paths,frames,HTTP_BUDGET+1)
    def test_missing_emitted_core_frames_fail_closed(self):
        for missing in ['mp_dispatch','mp_cached','service_command_mp',
                        'phv_parse','device_status_mp','encode_a1',
                        'a1_status_capture','phw_cached_value','ph_wire_malloc']:
            frames,paths=self.fixture();del frames[missing]
            with self.subTest(missing=missing),redirect_stdout(io.StringIO()):
                with self.assertRaisesRegex(ValueError,'missing actual .su'):
                    validate_paths(paths,frames)
        frames,paths=self.fixture();symbols=dict.fromkeys(frames,1);del frames['phv_scan']
        with self.assertRaisesRegex(ValueError,'Emitted helper missing'):
            http_paths(frames,'A1',symbols)
    def test_inlined_private_helper_is_in_caller_frame(self):
        frames,paths=self.fixture();del frames['phv_scan']
        paths=http_paths(frames,'A1',frames)
        with redirect_stdout(io.StringIO()):validate_paths(paths,frames)
        self.assertTrue(any('phv_parse' in names for reserve,names in paths.values()))
    def test_overbudget_and_weakened_reserve_fail(self):
        frames,paths=self.fixture();frames['service_product_status_mp']=10000
        with redirect_stdout(io.StringIO()),self.assertRaisesRegex(ValueError,'>12288'):
            validate_paths(paths,frames)
        with self.assertRaisesRegex(ValueError,'insufficient SDK'):
            validate_paths({'bad':(HTTP_RESERVE-1,['owner'])},{'owner':1})
    def test_capture_not_retained_during_encoding_and_cache_hit_no_parse(self):
        frames,paths=self.fixture()
        capture=paths['HTTP GET full status capture'][1]
        self.assertIn('a1_status_capture',capture);self.assertNotIn('device_status_mp',capture)
        for key,(reserve,names) in paths.items():
            if key.startswith('HTTP GET full status wire'):
                self.assertIn('device_status_mp',names);self.assertNotIn('a1_status_capture',names)
        cached=paths['HTTP GET cached status copy'][1]
        self.assertIn('phw_cached_value',cached)
        self.assertFalse(set(cached)&{'phv_parse','service_product_status_mp','device_status_mp'})
        with self.assertRaisesRegex(ValueError,'JSON compatibility'):
            status_paths(frames,'A1','bad',[],3072,compat=True)
    def test_symbols_require_real_core_and_forbid_json_bridges(self):
        required=['ph_http_server_step','ph_http_parser_feed','ph_http_start',
            'ph_mqtt_session_run','ph_mqtt_poll','ph_mqtt_enqueue_generation',
            'service_command_mp','service_query_mp','service_product_mp',
            'service_product_status_mp','device_status_mp','phv_parse',
            'phw_cached_value','phw_number','ph_buf_reserve']
        symbols=linked_symbols('\n'.join('00001000 T '+n for n in required))
        check_symbols(symbols)
        # HA may still link JSON. Only product/status graph bridges are banned.
        check_symbols(dict(symbols,json_object_new_object=1))
        for name in ['service_command_wire','ph_binary_parse','device_status_encode','http_worker']:
            with self.subTest(name=name),self.assertRaisesRegex(ValueError,'bridge linked'):
                check_symbols(dict(symbols,**{name:1}))
        for name in required:
            missing=dict(symbols);del missing[name]
            with self.subTest(missing=name),self.assertRaisesRegex(ValueError,'Expected one linked'):
                check_symbols(missing)
        with self.assertRaises(ValueError):check_symbols(dict(symbols,phv_parse=2))
    def test_ota_progress_decimal_closure_keeps_worker_reserve(self):
        names=['download_worker','ota_fetch','fetch_write','OtaServerStatusHandler',
               'ph_decimal_format_float32','pd_format','pd_mul']
        frames=dict.fromkeys(names,100)
        with redirect_stdout(io.StringIO()):check_tc1_ota_decimal(frames,4096,frames)
        absent=dict(frames);del absent['pd_format']
        with self.assertRaisesRegex(ValueError,'missing .su'):
            check_tc1_ota_decimal(absent,4096,absent)
        frames['pd_format']=1500
        with self.assertRaisesRegex(ValueError,'>4096'):
            check_tc1_ota_decimal(frames,4096,frames)

    def test_stack_files_dynamic_and_split_verification(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);(root/'TC1').mkdir()
            source=root/'TC1'/'fixture.su'
            source.write_text('app.c:1:1:tc1_ota_verify\t8\tstatic\napp.c:2:1:tc1_ota_verify.part.0\t632\tstatic\napp.c:3:1:phv_scan.constprop.0\t512\tstatic\n')
            frames=read_frames(root,'TC1')
            self.assertEqual(frames['tc1_ota_verify'],640);self.assertEqual(frames['phv_scan'],512)
            source.write_text('app.c:1:1:service_command_mp\t64\tdynamic\n')
            with self.assertRaisesRegex(ValueError,'Unbounded/nonstatic'):read_frames(root,'TC1')

if __name__=='__main__':unittest.main()
