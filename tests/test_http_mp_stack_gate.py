"""Negative fixtures for the strict linked MessagePack HTTP stack gate."""
from contextlib import redirect_stdout
import io
from pathlib import Path
import tempfile
import unittest
from tools.check_http_core import (HTTP_BUDGET, HTTP_RESERVE, check_symbols,
    HTTP_MODEL_BUDGETS, model_http_budget, http_paths, linked_symbols, read_frames,
    validate_paths, check_tc1_ota_decimal, check_tc1_ota_progress)
from tools.status_stack import status_paths, diagnostic_paths

class HTTPMessagePackStackGate(unittest.TestCase):
    def test_model_stack_budgets_keep_reserve_and_reject_oversized_paths(self):
        root=Path(__file__).resolve().parents[1]
        for model,budget in HTTP_MODEL_BUDGETS.items():
            self.assertEqual(model_http_budget(root,model),budget)
            frames={'capture':budget-HTTP_RESERVE}
            paths={'capture':(HTTP_RESERVE,['capture'])}
            with redirect_stdout(io.StringIO()):validate_paths(paths,frames,budget,model)
            frames['capture']+=1
            with self.assertRaisesRegex(ValueError,'>'+str(budget)):
                validate_paths(paths,frames,budget,model)
            if budget!=HTTP_BUDGET:
                with self.assertRaisesRegex(ValueError,'budget must remain'):
                    validate_paths(paths,frames,HTTP_BUDGET,model)
            with self.assertRaisesRegex(ValueError,'insufficient SDK'):
                validate_paths({'capture':(HTTP_RESERVE-1,[])},{},budget,model)

    def fixture(self,model='A1'):
        # First discover required public/noinline frames. Emit all known
        # private helpers as a separate second pass, simulating un-inlining.
        paths=http_paths({},model)
        frames={name:16 for reserve,names in paths.values() for name in names}
        for name in ['mp_decode','phv_scan','phv_next','phv_duplicate','phv_head',
                     'phv_skip_validated','binary_token','phw_bytes','phw_prefix','install_config',
                     'phw_quote','phw_head','device_status_visit','emit_string',
                     'runtime_fields','mp_copy','mp_cache_evict','memory_response','acquire',
                     'action','read_body','close_slot','respond','transmit',
                     'mp_dispatch','mp_query','ph_http_service_status','mp_receipt_save',
                     'mp_receipt_copy','mp_receipt_release','mp_receipt_difference',
                     'mp_receipt_apply','mp_receipt_u16','mp_receipt_put_u16',
                     'keys','number','boolparam','strparam','tc1_mp_get','tc1_mp_has']:
            frames[name]=16
        return frames,http_paths(frames,model,frames)
    def test_actual_mp_paths_and_unchanged_budget(self):
        for model in ['A1','M1','TC1']:
            frames,paths=self.fixture(model)
            with redirect_stdout(io.StringIO()):validate_paths(paths,frames)
            names={n for reserve,stack in paths.values() for n in stack}
            self.assertTrue({'mp_dispatch_authenticated','mp_cached','service_product_status_mp',
                             'device_status_mp','phv_scan','phw_cached_value','mp_cache_evict'}<=names)
            self.assertTrue(all(reserve>=3072 for reserve,names in paths.values()))
            self.assertFalse(any('json' in n for n in names))
            with self.assertRaisesRegex(ValueError,'budget must remain'):
                validate_paths(paths,frames,HTTP_BUDGET+1)
    def test_diagnostic_entrypoints_are_charged_and_cannot_hide_overflow(self):
        for missing in ['device_diagnostics_mp','service_product_diagnostics_mp','a1_diagnostics_capture']:
            frames,paths=self.fixture();del frames[missing]
            with redirect_stdout(io.StringIO()),self.assertRaisesRegex(ValueError,'missing actual .su'):
                validate_paths(paths,frames)
        frames,paths=self.fixture();frames['device_diagnostics_mp']=10000
        with redirect_stdout(io.StringIO()),self.assertRaisesRegex(ValueError,'>12288'):
            validate_paths(paths,frames)

    def test_tc1_diagnostics_charge_flag_reader_and_reset_helpers(self):
        helpers=['bemfa_status_capture','bemfa_config_flags_get',
                 'tc1_status_begin','tc1_ota_snapshot','reset_name','device_status_text']
        frames=dict.fromkeys(helpers,16)
        paths=diagnostic_paths(frames,'TC1','diagnostics',[],3072,frames)
        names=paths['diagnostics business capture'][0]
        self.assertTrue(set(helpers)<=set(names))
        self.assertNotIn('bemfa_config_get',names)
        missing=dict(frames);del missing['bemfa_config_flags_get']
        with self.assertRaisesRegex(ValueError,'Emitted helper missing'):
            diagnostic_paths(missing,'TC1','diagnostics',[],3072,frames)

    def test_missing_emitted_core_frames_fail_closed(self):
        for missing in ['mp_dispatch_authenticated','mp_cached','service_http_command_mp',
                        'phv_parse','device_status_mp','encode_a1',
                        'a1_status_capture','phw_cached_value','ph_wire_malloc','feature_light_select_mp','feature_mp']:
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

    def test_tc1_business_branches_and_http_wrappers_follow_real_calls(self):
        frames,paths=self.fixture('TC1')
        with redirect_stdout(io.StringIO()):validate_paths(paths,frames,8192,'TC1')
        generic=paths['HTTP POST business generic string'][1]
        control=paths['HTTP POST business control number scan'][1]
        self.assertIn('tc1_product_inner_mp',generic);self.assertNotIn('tc1_control_mp',generic)
        self.assertIn('tc1_control_mp',control);self.assertNotIn('tc1_product_inner_mp',control)
        self.assertIn('service_http_command_mp',control);self.assertIn('mp_dispatch_authenticated',control)
        self.assertNotIn('mp_dispatch',control);self.assertNotIn('service_command_mp',control)
        self.assertEqual(control.count('phv_get'),1);self.assertNotIn('boolparam',control)
        metadata=paths['HTTP GET service_product_info_mp string'][1]
        self.assertIn('service_http_query_mp',metadata);self.assertIn('mp_query',metadata)
        self.assertFalse(set(metadata)&{'tc1_product_inner_mp','tc1_control_mp'})
        error=paths['HTTP GET service status phv_parse'][1]
        self.assertIn('ph_http_service_status',error)
        self.assertFalse(set(error)&{'mp_query','mp_dispatch_authenticated','service_http_query_mp'})

    def test_tc1_new_frames_and_receipt_helpers_cannot_hide_stack_overflow(self):
        for name in ['tc1_product_inner_mp','tc1_control_mp','service_http_query_mp',
                     'service_http_command_mp','mp_dispatch_authenticated']:
            frames,paths=self.fixture('TC1');del frames[name]
            with self.subTest(name=name),redirect_stdout(io.StringIO()),self.assertRaisesRegex(ValueError,'missing actual .su'):
                validate_paths(paths,frames,8192,'TC1')
            frames,paths=self.fixture('TC1');frames[name]=5200
            with self.subTest(name=name),redirect_stdout(io.StringIO()),self.assertRaisesRegex(ValueError,'>8192'):
                validate_paths(paths,frames,8192,'TC1')
        for name in ['mp_query','ph_http_service_status','mp_receipt_save',
                     'mp_receipt_difference','mp_receipt_copy','mp_receipt_apply','mp_receipt_release']:
            frames,paths=self.fixture('TC1');symbols=dict(frames);del frames[name]
            with self.subTest(name=name),self.assertRaisesRegex(ValueError,'Emitted helper missing'):
                http_paths(frames,'TC1',symbols)
        frames,paths=self.fixture('TC1')
        receipt=paths['HTTP POST receipt save difference'][1]
        self.assertTrue({'mp_receipt_save','mp_receipt_difference'}<=set(receipt))
        self.assertFalse(set(receipt)&{'mp_receipt_copy','mp_receipt_release','ph_wire_malloc'})
        frames['mp_receipt_save']=5200
        with redirect_stdout(io.StringIO()),self.assertRaisesRegex(ValueError,'>8192'):
            validate_paths(paths,frames,8192,'TC1')
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
    def test_cold_sizing_charges_actual_writer_and_visitor_siblings(self):
        frames,paths=self.fixture('TC1')
        cold='HTTP GET full status cold sizing wire double number'
        regular='HTTP GET full status wire double number'
        self.assertEqual(paths[cold],paths[regular])
        self.assertEqual(paths[cold][1].count('device_status_visit'),1)
        self.assertNotIn('tc1_status_capture',paths[cold][1])
        for api,branch in [('phw_init_measure','init'),('phw_finish_fixed','finish')]:
            broken=dict(frames);del broken[api]
            with redirect_stdout(io.StringIO()),self.assertRaisesRegex(ValueError,'missing actual .su'):
                validate_paths(paths,broken)
            oversized=dict(frames);oversized[api]=10000
            with redirect_stdout(io.StringIO()),self.assertRaisesRegex(ValueError,'>12288'):
                validate_paths(paths,oversized)
            self.assertIn(api,paths['HTTP GET full status cold sizing '+branch][1])
        oversized=dict(frames);oversized['device_status_visit']=10000
        with redirect_stdout(io.StringIO()),self.assertRaisesRegex(ValueError,'>12288'):
            validate_paths({cold:paths[cold]},oversized)

    def test_symbols_require_real_core_and_forbid_json_bridges(self):
        required=['ph_http_server_step','ph_http_parser_feed','ph_http_start',
            'ph_mqtt_session_run','ph_mqtt_poll','ph_mqtt_enqueue_generation',
            'service_command_mp','service_query_mp','service_product_mp',
            'service_http_command_mp','service_http_query_mp',
            'service_product_status_mp','service_product_diagnostics_mp','device_diagnostics_mp','phv_parse',
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

    def test_integer_ota_progress_counts_logging_but_not_decimal(self):
        frames={'download_worker':32,'ota_fetch':1896,'fetch_write':176,
                'OtaServerStatusHandler':24,'WebLog':192}
        with redirect_stdout(io.StringIO()):check_tc1_ota_progress(frames,6144)
        absent=dict(frames);del absent['WebLog']
        with self.assertRaisesRegex(ValueError,'missing .su'):
            check_tc1_ota_progress(absent,6144)
        frames['WebLog']=2000
        with self.assertRaisesRegex(ValueError,'>6144'):
            check_tc1_ota_progress(frames,6144)

    def test_stack_files_dynamic_and_split_verification(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);(root/'TC1').mkdir()
            source=root/'TC1'/'fixture.su'
            source.write_text('app.c:1:1:tc1_ota_verify\t8\tstatic\napp.c:2:1:tc1_ota_verify.part.0\t632\tstatic\napp.c:3:1:phv_scan.constprop.0\t512\tstatic\n')
            frames=read_frames(root,'TC1')
            self.assertEqual(frames['tc1_ota_verify'],640);self.assertEqual(frames['phv_scan'],512)
            source.write_text('app.c:1:1:service_command_mp\t64\tdynamic\n')
            frames=read_frames(root,'TC1')
            with self.assertRaisesRegex(ValueError,'unbounded/nonstatic'):
                validate_paths({'actual path':(3072,['service_command_mp'])},frames)
            with redirect_stdout(io.StringIO()):
                validate_paths({'other owner':(3072,[])},frames)

if __name__=='__main__':unittest.main()
