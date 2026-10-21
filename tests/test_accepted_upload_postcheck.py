"""Portable synthetic evidence with real P256; optional real incident is opt-in."""
import copy
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch
from cryptography.hazmat.primitives.asymmetric import ec
from tools.ota import package
from tests.device import accepted_upload_postcheck as V


class EvidenceFixture:
    def __init__(self, root):
        self.root = root
        self.private = root/'build/status101/private'
        self.deploy = self.private/('deploy-'+V.LABEL+'-initial-111')
        self.inner_dir = self.private/(V.LABEL+'-initial-controller')
        self.outer_dir = self.private/(V.LABEL+'-diag-mqtt-failure')
        self.post_dir = self.private/(V.LABEL+'-upload-postcheck')
        self.cleanup_file = self.post_dir/'cleanup-proof.json'
        def git(*args):
            return subprocess.check_output(['git', *args], cwd=root, text=True, stderr=subprocess.DEVNULL).strip()
        git('init')
        self.put(root/'firmware.c', b'firmware')
        git('add', 'firmware.c')
        git('-c', 'user.email=test@example.invalid', '-c', 'user.name=Test', 'commit', '-m', 'candidate')
        self.commit = git('rev-parse', 'HEAD')
        self.key = ec.generate_private_key(ec.SECP256R1())
        numbers = self.key.public_key().public_numbers()
        public = numbers.x.to_bytes(32, 'big')+numbers.y.to_bytes(32, 'big')
        self.put(root/'common/ota/keyring.h', ('{1,{'+','.join('0x%02x'%b for b in public)+'}}').encode())
        self.put(root/'tools/ota/package.py', Path(package.__file__).read_bytes())
        controls = dict(requested_fan_on=False, requested_speed=100, child_lock=False, led_enabled=True)
        self.status = dict(controls, device_id=V.IDENT, model='a1', version=V.VERSION,
                           uptime_seconds=20, firmware_variant='full', full_resources=True, bootstrap_ui=False,
                           wifi_connected=True, mqtt_connected=True, ota_active=False, reboot_pending=False,
                           ota_state='idle', ota_error='')
        self.config = {'name': 'unchanged', 'mqtt_host': 'fixture.invalid'}
        before = {'status': dict(self.status, version=V.PREVIOUS, uptime_seconds=4491), 'config': self.config}
        self.put(self.deploy/'before.json', before)
        hashes, paths, manifests = {}, {}, {}
        for model in ('a1', 'tc1', 'm1'):
            folder = root/'build/status101/candidates'/V.ARCHIVE/model
            name = 'phicomm_'+('tc1_a1' if model == 'tc1' else model)+'.ota.bin'
            raw = (model+' payload').encode()
            signed = package.sign(raw, model, '0.9.15', self.commit, self.key)
            manifest = dict(git_commit=self.commit, version=V.VERSION, base_version='0.9.15', dirty=False,
                            firmware_variant='full', target='A1@MK3080B@moc' if model == 'a1' else model,
                            files={name: V.digest(raw)})
            paths[model] = {'manifest': str(folder/'manifest.json'), 'raw': str(folder/name), 'signed': str(folder/(name+'.signed'))}
            hashes[model] = dict(manifest_sha256=self.put(paths[model]['manifest'], manifest),
                                 raw_sha256=self.put(paths[model]['raw'], raw),
                                 signed_sha256=self.put(paths[model]['signed'], signed))
            manifests[model] = manifest
        a1_signed = Path(paths['a1']['signed']).read_bytes()
        intent = dict(ip=V.IP, device_id=V.IDENT, at=1100, sha256=hashes['a1']['signed_sha256'], manifest=manifests['a1'])
        self.put(self.deploy/'upload-intent.json', intent)
        self.put(self.deploy/'upload.json', dict(http=202, body=json.dumps({'message':'OTA verified; rebooting'}), failed=False,
            request=dict(request_failed=False, request_exception=None, started_monotonic=10,
                         ended_monotonic=20, response_read_monotonic=30, body_bytes=len(a1_signed))))
        source_records = {}
        for name in ('controller.py', 'deploy_candidate.py', 'a1_ota_trace.py', 'audit_mixed_candidate_completion.py',
                     'diagnose_shared_candidate.py', 'readonly_stress.py', 'status_benchmark.py',
                     'validate_shared_candidate.py', 'ota_package.py'):
            data = Path(package.__file__).read_bytes() if name == 'ota_package.py' else b'archived '+name.encode()
            relative = 'sources/'+name
            sha = self.put(self.inner_dir/relative, data)
            source_records[name] = dict(archive_path=relative, sha256=sha, archived_sha256=sha)
        source_manifest = dict(candidate_commit=self.commit, candidate_version=V.VERSION, sources=source_records)
        manifest_sha = self.put(self.inner_dir/'source-manifest.json', source_manifest)
        step = dict(name=V.LABEL+'-initial-deploy-111', argv=['python', 'deploy_candidate.py','111', V.LABEL+'-initial', V.PREVIOUS,V.ARCHIVE],
                    exit_code=1, started_at=1000, ended_at=1200, started_monotonic=0, ended_monotonic=50)
        stage = dict(pid=99, exit_code=1, cleanup=dict(groups_empty=True, remaining={}, signals=[]),
                     timed_out=False, interrupted=None, disk_blocked=False, log=str(self.outer_dir/'initial.log'))
        self.inner = dict(status='failed', failure_type='CaptureCleanupError',
            failure='RuntimeError: Could not determine owned capture container state', completed=False,
            initial_signed_ota_complete=False, paced_diagnostic_complete=False, source_unchanged=True,
            source_copies_unchanged=True, source_manifest_unchanged=True, tracked_worktree_clean=True,
            firmware_commit=self.commit, version=V.VERSION, archive=V.ARCHIVE, previous_version=V.PREVIOUS,
            steps=[step], pid=99, source_manifest_sha256=manifest_sha,
            archived_hashes_at_end={n:r['sha256'] for n,r in source_records.items()},
            source_hashes_at_end={n:r['sha256'] for n,r in source_records.items()})
        self.outer = dict(status='failed', candidate_unchanged=True, head_unchanged=True, source_unchanged=True,
            candidate_hashes=hashes, candidate_hashes_at_end=hashes,
            frozen=dict(candidate=hashes, head=self.commit, candidate_paths=paths),
            source_hashes_at_start={'initial':'same'}, source_hashes_at_end={'initial':'same'},
            candidate_commit=self.commit, version=V.VERSION, archive=V.ARCHIVE, previous_version=V.PREVIOUS, stages=[stage])
        inner_sha = self.put(self.inner_dir/'controller.json', self.inner)
        outer_sha = self.put(self.outer_dir/'controller.json', self.outer)
        caps = dict(model='a1', firmware_variant='full', full_resources=True, bootstrap_ui=False, status_views=['full','live'])
        memory = dict(http_core_owner_state=1, http_core_restarts=0, http_core_close_failures=0,
                      mqtt_errors=0, mqtt_reconnects=0, free_memory=27000)
        events = []
        for i, (endpoint, data) in enumerate(zip(('status','config','capabilities','memory'), (self.status,self.config,caps,memory))):
            path = self.post_dir/'raw'/(endpoint+'.bin')
            sha = self.put(path, dict(code='ok', data=data))
            events.append(dict(endpoint='/api/v1/'+endpoint, http=200, outcome='success', raw_file=str(path),
                               raw_sha256=sha, raw_bytes=path.stat().st_size, request_start_mono=100+i*0.1,
                               request_end_mono=100+i*0.1+0.05))
        post = dict(status='observed', overall_health_gate='PASS', r2_overall_status='FAILED',
                    request_count=4,max_requests=4, started_at=2000,ended_at=2000.5,
                    started_monotonic=100,elapsed_seconds=0.5, events=events,
                    baseline_sha256=V.digest((self.deploy/'before.json').read_bytes()))
        post_sha = self.put(self.post_dir/'evidence.json', post)
        capture_sha = self.put(self.deploy/'a1-ota-trace/capture.json', dict(container_name=V.CAPTURE,
                    upload_attempt_finished=True,status='failed',owned_container_cleanup=None))
        capture_intent_sha = self.put(self.deploy/'a1-ota-trace/capture-intent.json', {'intent':'capture'})
        helper_sha = self.put(root/'build/status101/postcheck_helper.py', b'helper')
        def binding(path, sha): return dict(file=str(path.relative_to(root)),sha256=sha)
        stderr = 'Error response from daemon: No such container: '+V.CAPTURE+'\n'
        self.cleanup = dict(schema='shared4e5a85-diag-r2-cleanup-proof-v1', created_at_epoch=2001,
            all_owned_process_groups_empty=True, all_pids_absent=True,
            old_owned_pids={str(p):dict(proc_present=False) for p in (129748,129751,129757,129762)},
            capture_container=dict(name=V.CAPTURE, original_capture_status='failed',
                intent_file=str((self.deploy/'a1-ota-trace/capture-intent.json').relative_to(root)), intent_sha256=capture_intent_sha,
                original_capture_result_file=str((self.deploy/'a1-ota-trace/capture.json').relative_to(root)), original_capture_result_sha256=capture_sha,
                inspect=dict(argv=['docker','inspect','--type','container',V.CAPTURE],returncode=1,outcome='absent',
                    stderr=stderr,stderr_sha256=V.digest(stderr.encode()),stdout_bytes=3,stdout_sha256=V.digest(b'[]\n'),elapsed_seconds=0.01)),
            original_initial_controller=binding(self.inner_dir/'controller.json',inner_sha),
            outer_controller_cleanup=dict(binding(self.outer_dir/'controller.json',outer_sha), groups_empty=True,remaining={},initial_stage_record=stage),
            postcheck=dict(evidence_file=str((self.post_dir/'evidence.json').relative_to(root)),evidence_sha256=post_sha,
                helper_file='build/status101/postcheck_helper.py',helper_sha256=helper_sha,
                readonly_request_helper_file='build/status101/postcheck_helper.py',readonly_request_helper_sha256=helper_sha))
        self.cleanup_sha = self.put(self.cleanup_file,self.cleanup)
        for path in (root/'build/status101'/(step['name']+'.log'), Path(stage['log']),self.deploy/'memory-before-upload.json',
                     self.deploy/'a1-ota-trace/a1-ota.pcap',self.deploy/'a1-ota-trace/a1-ota.tcp.txt'):
            self.put(path,b'evidence')
        self.anchors = {name: V.digest((self.private/name).read_bytes()) for name in V.ANCHORS}

    @staticmethod
    def put(path,data):
        path=Path(path)
        path.parent.mkdir(parents=True,exist_ok=True)
        raw=data if isinstance(data,bytes) else (json.dumps(data,sort_keys=True)+'\n').encode()
        path.write_bytes(raw)
        return V.digest(raw)


class AcceptedEvidenceTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.fixture=EvidenceFixture(Path(self.temp.name))
        for key,value in (('COMMIT',self.fixture.commit),('ANCHORS',self.fixture.anchors),('CLEANUP_SHA256',self.fixture.cleanup_sha)):
            p=patch.object(V,key,value);p.start();self.addCleanup(p.stop)

    def verify(self,**kwargs):
        return V.verify_accepted_upload_postcheck(self.fixture.cleanup_file,root=self.fixture.root,**kwargs)

    def test_portable_real_p256_all_models_and_failed_history(self):
        proof=self.verify()
        self.assertEqual(proof['schema'],V.SCHEMA)
        self.assertEqual(proof['original_failed_step']['exit_code'],1)
        self.assertEqual(proof['original_inner_status'],'failed')
        self.assertEqual(proof['original_outer_status'],'failed')
        self.assertEqual(proof['original_step']['name'],'independent-upload-postcheck')
        self.assertFalse((self.fixture.deploy/'after.json').exists())
        self.assertGreater(len(proof['proof_sha256']),30)

    def semantic_tamper(self,predicate,mutate):
        original=V.load_json
        hits=[]
        def read(data):
            obj=original(data)
            if predicate(obj):hits.append(True);mutate(obj)
            return obj
        with patch.object(V,'load_json',side_effect=read),self.assertRaises((ValueError,KeyError)):
            self.verify()
        self.assertTrue(hits)

    def test_identity_config_controls_health_and_reboot_negatives(self):
        status=lambda o:o.get('code')=='ok' and o.get('data',{}).get('device_id')==V.IDENT
        for key,value in [('device_id','other'),('version',V.PREVIOUS),('model','m1'),('requested_speed',99),
                          ('mqtt_connected',False),('ota_active',True),('uptime_seconds',9000),('uptime_seconds',float('inf')),('full_resources',False)]:
            with self.subTest(key=key):self.semantic_tamper(status,lambda o,k=key,x=value:o['data'].__setitem__(k,x))
        self.semantic_tamper(lambda o:'mqtt_host' in o.get('data',{}),lambda o:o['data'].__setitem__('name','changed'))
        self.semantic_tamper(lambda o:'http_core_owner_state' in o.get('data',{}),lambda o:o['data'].pop('http_core_owner_state'))
        self.semantic_tamper(lambda o:'status_views' in o.get('data',{}),lambda o:o['data'].__setitem__('status_views',['full']))

    def test_acceptance_failure_cleanup_source_negatives(self):
        for predicate,mutate in (
            (lambda o:'request' in o,lambda o:o.__setitem__('http',400)),
            (lambda o:'failure_type' in o,lambda o:o.__setitem__('status','complete')),
            (lambda o:'candidate_commit' in o and 'sources' in o,lambda o:o.__setitem__('candidate_commit','0'*40)),
            (lambda o:'all_pids_absent' in o,lambda o:o['old_owned_pids']['129762'].__setitem__('proc_present',True)),
            (lambda o:'all_pids_absent' in o,lambda o:o.__setitem__('all_owned_process_groups_empty',False)),
            (lambda o:o.get('overall_health_gate')=='PASS',lambda o:o['events'].pop())):
            self.semantic_tamper(predicate,mutate)

    def test_raw_signed_archive_cleanup_tamper(self):
        paths=[self.fixture.cleanup_file,self.fixture.post_dir/'raw/status.bin',
               self.fixture.inner_dir/'sources/controller.py',
               self.fixture.root/'build/status101/candidates'/V.ARCHIVE/'a1/phicomm_a1.ota.bin.signed']
        for path in paths:
            old=path.read_bytes();path.write_bytes(old+b'x')
            try:
                with self.assertRaises(ValueError):self.verify()
            finally:path.write_bytes(old)

    def test_invalid_p256_not_just_hash_and_production_tree_drift(self):
        signed=bytearray((self.fixture.root/'build/status101/candidates'/V.ARCHIVE/'a1/phicomm_a1.ota.bin.signed').read_bytes())
        signed[128]^=1
        with self.assertRaisesRegex(ValueError,'signature'):
            package.verify_package(signed,'a1',self.fixture.root/'common/ota/keyring.h')
        (self.fixture.root/'firmware.c').write_text('changed')
        with self.assertRaisesRegex(ValueError,'source changed'):self.verify()

    def test_historical_validation_head_is_stable_after_docs_commit(self):
        old=self.verify()
        root=self.fixture.root
        self.fixture.put(root/'docs/check.md',b'new doc')
        def git(*args):return subprocess.check_output(['git',*args],cwd=root,text=True).strip()
        git('add','docs/check.md')
        git('-c','user.email=test@example.invalid','-c','user.name=Test','commit','-m','docs')
        replay=self.verify(validation_commit=self.fixture.commit,check_current=False)
        self.assertEqual(old,replay)
        current=self.verify()
        self.assertNotEqual(current['validation_commit'],old['validation_commit'])
        with self.assertRaisesRegex(ValueError,'not current'):
            self.verify(validation_commit=self.fixture.commit)


@unittest.skipUnless(os.environ.get('ACCEPTED_UPLOAD_REAL_ROOT'),'explicit private evidence opt-in')
class RealIncidentTests(unittest.TestCase):
    def test_actual_incident(self):
        root=Path(os.environ['ACCEPTED_UPLOAD_REAL_ROOT']).resolve()
        proof=V.verify_accepted_upload_postcheck(root/'build/status101/private'/ (V.LABEL+'-upload-postcheck/cleanup-proof.json'),root=root)
        self.assertEqual(proof['commit'],V.COMMIT)
