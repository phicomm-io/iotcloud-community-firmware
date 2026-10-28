"""Evidence must prove a complete exact-source main run; no network/builds."""
import copy
import importlib.util
import io
import json
from pathlib import Path
import re
import tempfile
import unittest
from unittest.mock import patch
import urllib.error
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('validation_reuse', ROOT / 'tools/ci/validation_reuse.py')
reuse = importlib.util.module_from_spec(spec)
spec.loader.exec_module(reuse)
REPO = 'phicomm-io/iotcloud-community-firmware'
FINGERPRINT = dict(source_commit='a' * 40, source_tree='b' * 40, frontend_lock_sha256='c' * 64,
    frontend_archive_sha256='d' * 64, protocols_lock_sha256='e' * 64, protocols_commit='f' * 40,
    workflow_sha256='1' * 64)


def needs(reused=False):
    value = {job: dict(result='success', outputs={}) for job in reuse.NEEDS}
    value['frontend']['outputs'] = {'reuse-heavy-validation': str(reused).lower(), 'validation-run-id': '123' if reused else ''}
    if reused:
        value['feature-tests']['result'] = 'skipped'
    return value


def context(**changes):
    return dict(repository=REPO, event='push', ref='refs/heads/main', run_id=123, run_attempt=2, **changes)


class FakeApi:
    def __init__(self):
        self.calls = []
        self.run = dict(id=123, run_attempt=2, status='completed', conclusion='success', event='push', head_branch='main',
            head_sha=FINGERPRINT['source_commit'], repository={'full_name': REPO}, head_repository={'full_name': REPO},
            workflow_id=9, path=reuse.WORKFLOW)
        self.workflow = dict(id=9, path=reuse.WORKFLOW)
        self.jobs = [dict(name=name, status='completed', conclusion='success',
            steps=[dict(name=step, conclusion='success') for step in steps]) for name, steps in reuse.JOBS.items()]
        self.artifacts = [dict(id=90, name=reuse.ARTIFACT, expired=False,
            workflow_run=dict(id=123, head_sha=FINGERPRINT['source_commit']))]
        self.candidates = [self.run]
        self.proof = reuse.create(needs(), context(), FINGERPRINT)
        self.zip_names = ['validation.json']
        self.raw_zip = None
        self.job_count = None
        self.artifact_count = None

    def get(self, path, binary=False):
        self.calls.append(path)
        if path.startswith('/actions/workflows/build.yml/runs?'):
            return {'workflow_runs': self.candidates}
        if path == '/actions/workflows/build.yml':
            return self.workflow
        if path == '/actions/runs/123':
            return self.run
        if path.endswith('/attempts/2/jobs?per_page=100'):
            return dict(total_count=len(self.jobs) if self.job_count is None else self.job_count, jobs=self.jobs)
        if path.endswith('/artifacts?per_page=100'):
            return dict(total_count=len(self.artifacts) if self.artifact_count is None else self.artifact_count, artifacts=self.artifacts)
        if path == '/actions/artifacts/90/zip':
            if self.raw_zip is not None:
                return self.raw_zip
            output = io.BytesIO()
            with zipfile.ZipFile(output, 'w') as zipped:
                for name in self.zip_names:
                    zipped.writestr(name, json.dumps(self.proof))
            return output.getvalue()
        raise AssertionError('Unexpected API request: ' + path)


class ValidationReuseTests(unittest.TestCase):
    def test_complete_main_evidence_and_tag_reuse(self):
        api = FakeApi()
        api.raw_zip = api.get('/actions/artifacts/90/zip', binary=True)
        api.artifacts[0]['digest'] = 'sha256:' + reuse.digest(api.raw_zip)
        result = reuse.resolve(FINGERPRINT, REPO, 'refs/tags/v1.0.0-rc.1', '', api)
        self.assertTrue(result['reused'])
        self.assertEqual((result['run_id'], result['run_attempt']), (123, 2))
        self.assertTrue(any('/attempts/2/jobs?' in call for call in api.calls))

    def test_explicit_valid_run(self):
        self.assertTrue(reuse.resolve(FINGERPRINT, REPO, 'refs/tags/v1.0.0', '123', FakeApi())['reused'])

    def test_main_dev_feature_and_pr_always_run_heavy_tests(self):
        for ref in ('refs/heads/main', 'refs/heads/dev', 'refs/heads/feature/new-interface', 'refs/pull/1/merge'):
            api = FakeApi()
            self.assertFalse(reuse.resolve(FINGERPRINT, REPO, ref, '', api)['reused'])
            self.assertEqual(api.calls, [])

    def test_explicit_invalid_or_non_tag_reuse_is_rejected(self):
        for value in ('0', '-1', '1x', '123\n', '01'):
            with self.subTest(value=value), self.assertRaises(reuse.Rejected):
                reuse.resolve(FINGERPRINT, REPO, 'refs/tags/v1.0.0', value, FakeApi())
        with self.assertRaises(reuse.Rejected):
            reuse.resolve(FINGERPRINT, REPO, 'refs/heads/main', '123', FakeApi())

    def test_no_evidence_falls_back_but_explicit_missing_is_rejected(self):
        api = FakeApi()
        api.artifacts = []
        self.assertFalse(reuse.resolve(FINGERPRINT, REPO, 'refs/tags/v1.0.0', '', api)['reused'])
        with self.assertRaises(reuse.Rejected):
            reuse.resolve(FINGERPRINT, REPO, 'refs/tags/v1.0.0', '123', api)

    def test_failed_incomplete_or_wrong_source_run_is_never_reused(self):
        mutations = [('head_sha', '0' * 40), ('head_branch', 'dev'), ('event', 'workflow_dispatch'),
            ('status', 'in_progress'), ('conclusion', 'failure'), ('repository', {'full_name': 'other/repo'}),
            ('head_repository', {'full_name': 'fork/repo'}), ('workflow_id', 10), ('path', 'other.yml'), ('run_attempt', 0)]
        for key, value in mutations:
            api = FakeApi()
            api.run[key] = value
            with self.subTest(key=key), self.assertRaises(reuse.Rejected):
                reuse.validate(123, FINGERPRINT, REPO, api)

    def test_workflow_identity_is_verified(self):
        api = FakeApi()
        api.workflow['path'] = '.github/workflows/unrelated.yml'
        with self.assertRaises(reuse.Rejected):
            reuse.validate(123, FINGERPRINT, REPO, api)

    def test_every_required_job_and_step_must_have_executed(self):
        for index, job in enumerate(FakeApi().jobs):
            for alteration in ('job-skipped', 'missing', 'step-skipped'):
                api = FakeApi()
                if alteration == 'missing':
                    api.jobs.pop(index)
                elif alteration == 'job-skipped':
                    api.jobs[index]['conclusion'] = 'skipped'
                else:
                    api.jobs[index]['steps'][0]['conclusion'] = 'skipped'
                with self.subTest(job=job['name'], alteration=alteration), self.assertRaises(reuse.Rejected):
                    reuse.validate(123, FINGERPRINT, REPO, api)

    def test_all_fingerprint_fields_are_bound(self):
        for key in FINGERPRINT:
            api = FakeApi()
            api.proof['fingerprint'] = dict(FINGERPRINT, **{key: '0' * len(FINGERPRINT[key])})
            with self.subTest(key=key), self.assertRaises(reuse.Rejected):
                reuse.validate(123, FINGERPRINT, REPO, api)

    def test_profile_repository_run_attempt_and_job_claim_are_bound(self):
        for key, value in [('schema', 2), ('schema', True), ('run_id', 123.0), ('run_attempt', 2.0),
                           ('profile', 'older'), ('repository', 'other/repo'), ('run_id', 124),
                           ('run_attempt', 1), ('event', 'pull_request'), ('branch', 'dev'), ('jobs', {})]:
            api = FakeApi()
            api.proof[key] = value
            with self.subTest(key=key), self.assertRaises(reuse.Rejected):
                reuse.validate(123, FINGERPRINT, REPO, api)

    def test_expired_duplicate_wrong_source_or_corrupt_artifact_rejected(self):
        for alteration in ('expired', 'duplicate', 'source', 'run', 'zip', 'layout', 'digest', 'jobs-page', 'artifacts-page'):
            api = FakeApi()
            if alteration == 'expired': api.artifacts[0]['expired'] = True
            if alteration == 'duplicate': api.artifacts *= 2
            if alteration == 'source': api.artifacts[0]['workflow_run']['head_sha'] = '0' * 40
            if alteration == 'run': api.artifacts[0]['workflow_run']['id'] = 456
            if alteration == 'zip': api.raw_zip = b'not a zip'
            if alteration == 'layout': api.zip_names = ['../validation.json']
            if alteration == 'digest': api.artifacts[0]['digest'] = 'sha256:' + '0' * 64
            if alteration == 'jobs-page': api.job_count = 101
            if alteration == 'artifacts-page': api.artifact_count = 101
            with self.subTest(alteration=alteration), self.assertRaises((reuse.Rejected, zipfile.BadZipFile)):
                reuse.validate(123, FINGERPRINT, REPO, api)

    def test_api_unavailability_falls_back_only_for_automatic_reuse(self):
        class Unavailable:
            def get(self, *args, **kwargs):
                raise urllib.error.URLError('synthetic unavailable')
        self.assertFalse(reuse.resolve(FINGERPRINT, REPO, 'refs/tags/v1.0.0', '', Unavailable())['reused'])
        with self.assertRaises(reuse.Rejected):
            reuse.resolve(FINGERPRINT, REPO, 'refs/tags/v1.0.0', '123', Unavailable())

    def test_malformed_metadata_falls_back_but_explicit_evidence_fails(self):
        for value in (None, [], 'invalid'):
            api = FakeApi()
            api.run = value
            api.candidates = [dict(id=123, head_sha=FINGERPRINT['source_commit'])]
            with self.subTest(value=value):
                self.assertFalse(reuse.resolve(FINGERPRINT, REPO, 'refs/tags/v1.0.0', '', api)['reused'])
                with self.assertRaises(reuse.Rejected):
                    reuse.resolve(FINGERPRINT, REPO, 'refs/tags/v1.0.0', '123', api)

    def test_gate_accepts_only_complete_builds_and_verified_heavy_result(self):
        self.assertFalse(reuse.gate_results(needs()))
        self.assertTrue(reuse.gate_results(needs(True)))
        for key in reuse.NEEDS:
            value = needs(True)
            value[key]['result'] = 'failure'
            with self.subTest(key=key), self.assertRaises(reuse.Rejected):
                reuse.gate_results(value)
        for value in (needs(), needs(True)):
            value['frontend']['outputs']['reuse-heavy-validation'] = 'false'
            value['feature-tests']['result'] = 'skipped'
            with self.assertRaises(reuse.Rejected): reuse.gate_results(value)
        value = needs(True)
        value['frontend']['outputs']['validation-run-id'] = ''
        with self.assertRaises(reuse.Rejected): reuse.gate_results(value)

    def test_evidence_cannot_be_minted_from_reused_or_non_main_runs(self):
        with self.assertRaises(reuse.Rejected): reuse.create(needs(True), context(), FINGERPRINT)
        for event, ref in [('workflow_dispatch', 'refs/heads/main'), ('push', 'refs/heads/dev'), ('push', 'refs/tags/v1.0.0')]:
            c = context()
            c.update(event=event, ref=ref)
            with self.assertRaises(reuse.Rejected): reuse.create(needs(), c, FINGERPRINT)

    def test_fingerprint_reads_real_lock_bytes_and_source_tree(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / '.github/workflows').mkdir(parents=True)
            (root / reuse.WORKFLOW).write_text('workflow')
            (root / 'frontend.lock.json').write_text(json.dumps(dict(schema=1, commit='a'*40, sha256='b'*64)))
            (root / 'protocols.lock.json').write_text(json.dumps(dict(schema=1, repository='phicomm-io/iotcloud-community-protocols', commit='c'*40)))
            with patch.object(reuse.subprocess, 'check_output', side_effect=['d'*40+'\n', 'e'*40+'\n']):
                value = reuse.fingerprint(root)
            self.assertEqual(value['source_commit'], 'd'*40)
            self.assertEqual(value['source_tree'], 'e'*40)
            self.assertEqual(value['frontend_lock_sha256'], reuse.digest((root/'frontend.lock.json').read_bytes()))
            self.assertEqual(value['protocols_commit'], 'c'*40)

    def test_redirect_never_sends_api_token_to_storage(self):
        request = urllib.request.Request('https://api.github.com/repos/example/repo/archive', headers={'Authorization': 'Bearer synthetic'})
        redirected = reuse.SafeRedirect().redirect_request(request, None, 302, '', {}, 'https://storage.example.invalid/artifact')
        self.assertIsNone(redirected.get_header('Authorization'))
        with self.assertRaises(reuse.Rejected):
            reuse.SafeRedirect().redirect_request(request, None, 302, '', {}, 'http://storage.example.invalid/artifact')

    def test_workflow_keeps_tag_compiles_actual_ota_and_release_gates(self):
        text = (ROOT / reuse.WORKFLOW).read_text()
        for name in ('Build full firmware', 'Build firmware without frontend resources', 'Build and collect A1 fresh-air variants',
                     'Build universal image and enforce stock OTA resource budgets', 'Test actual OTA handler, MessagePack wire and device sessions',
                     'Validate pinned protocol contracts', 'Prepare exact frontend archive', 'Test signed OTA vectors and Flash faults'):
            match = re.search(r'^      - name: ' + re.escape(name) + r'\n(.*?)(?=^      - |\Z)', text, re.M | re.S)
            self.assertIsNotNone(match, name)
            self.assertNotIn('reuse-heavy-validation', match[1], name)
        self.assertIn('needs.validation-evidence.result == \'success\'', text)
        self.assertIn('python3 tools/release/verify_user_notes_release.py', text)
        self.assertIn("if: needs.frontend.outputs.reuse-heavy-validation != 'true'", text)
        jobs = dict(re.findall(r'^  ([\w-]+):\n(.*?)(?=^  [\w-]+:|\Z)', text, re.M | re.S))
        owners = {
            'Verify locked frontend dependency': 'frontend',
            'tc1 firmware variants': 'firmware', 'a1 firmware variants': 'firmware', 'm1 firmware variants': 'firmware',
            'DAIKIN ESP32 full firmware': 'esp', 'DAIKIN ESP32 hub firmware': 'esp',
            'Lightweight release checks': 'release-checks',
            'Binding and automation host scenarios': 'feature-tests',
            'Validate complete test evidence': 'validation-evidence',
        }
        self.assertEqual(set(owners), set(reuse.JOBS))
        for job, steps in reuse.JOBS.items():
            for name in steps:
                matches = re.findall(r'^      - name: ' + re.escape(name) + r'$', jobs[owners[job]], re.M)
                self.assertEqual(len(matches), 1, (job, name))
        ota = re.search(r'^      - name: Test actual OTA handler, MessagePack wire and device sessions\n'
                        r'(.*?)(?=^      - |\Z)', jobs['esp'], re.M | re.S)
        self.assertIsNotNone(ota)
        self.assertNotRegex(ota[1], r'(?m)^        if:')
        self.assertIn('        variant: [full, hub]', jobs['esp'])
        for module in ('test_ota_identity.py', 'test_wire_msgpack.py', 'test_web_auth.py',
                       'test_lan_pairing.py', 'test_auth_storage.py', 'test_hub_mixed_devices.py'):
            self.assertIn("          python3 -m unittest discover -s DAIKIN/tests -p '" + module + "'", ota[1])

    def test_hub_validation_requires_udp_and_management_without_embedded_broker(self):
        text = (ROOT / reuse.WORKFLOW).read_text()
        self.assertIn('DAIKIN.tests.test_hub_lan_client DAIKIN.tests.test_hub_lan_home DAIKIN.tests.test_hub_lan_discovery', text)
        self.assertIn('python3 -m unittest DAIKIN.tests.test_lan_business', text)
        self.assertIn('python3 -m unittest DAIKIN.tests.test_pair_route', text)
        self.assertIn('python3 -m unittest DAIKIN.tests.test_local_control_priority', text)
        self.assertIn('python3 -m unittest DAIKIN.tests.test_hub_lifecycle', text)
        self.assertIn('node tests/hub-lan-management.mjs', text)
        self.assertIn('tests/run_mqtt_v2.sh', text)  # external-broker SDK coverage remains
        self.assertNotIn('tests/hub/console.cpp', text)
        self.assertNotIn('node tests/hub-mqtt-interop.mjs', text)
        self.assertNotIn('HubTests.test_actual_sdk_workers_share_broker_and_home_assistant', text)

    def assert_release_scheduling(self, text):
        """Guard the real YAML condition; do not emulate GitHub's scheduler."""
        job = re.search(r'^  release:\n(.*?)(?=^  [\w-]+:|\Z)', text, re.M | re.S)
        self.assertIsNotNone(job)
        self.assertRegex(job[1], re.compile(r'^    needs: \[sign-ota\]$', re.M))
        condition = re.search(r'^    if: (.*?)(?=^    [\w-]+:|\Z)', job[1], re.M | re.S)
        self.assertIsNotNone(condition)
        expression = condition[1].strip().removeprefix('>-').strip()
        self.assertEqual(' '.join(expression.split()),
            "always() && needs.sign-ota.result == 'success' && "
            "github.event_name == 'push' && github.ref_type == 'tag'")
        return job[0]

    def test_release_allows_signed_tag_after_a_skipped_ancestor(self):
        text = (ROOT / reuse.WORKFLOW).read_text()
        job = self.assert_release_scheduling(text)
        # The original tag-only condition inherited implicit success() and skipped
        # publication after verified reuse. It must fail this structural guard.
        old = re.sub(r'    if: >-\n.*?(?=^    [\w-]+:)',
            "    if: github.event_name == 'push' && github.ref_type == 'tag'\n",
            job, count=1, flags=re.M | re.S)
        self.assertNotEqual(old, job)
        with self.assertRaises(AssertionError):
            self.assert_release_scheduling(text.replace(job, old, 1))

    def test_release_guard_rejects_failed_skipped_or_unchecked_signing(self):
        text = (ROOT / reuse.WORKFLOW).read_text()
        job = self.assert_release_scheduling(text)
        guard = "needs.sign-ota.result == 'success' &&"
        for replacement in ("needs.sign-ota.result == 'failure' &&",
                            "needs.sign-ota.result == 'skipped' &&", ''):
            changed = job.replace(guard, replacement, 1)
            self.assertNotEqual(changed, job)
            with self.subTest(signing=replacement), self.assertRaises(AssertionError):
                self.assert_release_scheduling(text.replace(job, changed, 1))

    def test_release_guard_rejects_non_tag_or_non_push_publication(self):
        text = (ROOT / reuse.WORKFLOW).read_text()
        job = self.assert_release_scheduling(text)
        for original, replacement in (
            ("github.event_name == 'push' &&", ''),
            ("github.event_name == 'push'", "github.event_name == 'workflow_dispatch'"),
            ("github.event_name == 'push'", "github.event_name == 'pull_request'"),
            ("&& github.ref_type == 'tag'", ''),
            ("github.ref_type == 'tag'", "github.ref_type == 'branch'"),
        ):
            changed = job.replace(original, replacement, 1)
            self.assertNotEqual(changed, job)
            with self.subTest(condition=replacement), self.assertRaises(AssertionError):
                self.assert_release_scheduling(text.replace(job, changed, 1))


if __name__ == '__main__':
    unittest.main()
