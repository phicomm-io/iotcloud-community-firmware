#!/usr/bin/env python3
"""Reuse heavy tests only from a successful main push of this exact source."""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import re
import subprocess
import urllib.error
import urllib.parse
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[2]
PROFILE = 'full-firmware-validation-v1'
ARTIFACT = 'full-validation-evidence'
WORKFLOW = '.github/workflows/build.yml'
NEEDS = ('frontend', 'firmware', 'esp', 'release-checks', 'feature-tests')
JOBS = {
    'Verify locked frontend dependency': ('Validate pinned protocol contracts',),
    'tc1 firmware variants': ('Build full firmware', 'Build firmware without frontend resources', 'Test TC1 protocol and HTTP regressions'),
    'a1 firmware variants': ('Build full firmware', 'Build firmware without frontend resources', 'Build and collect A1 fresh-air variants'),
    'm1 firmware variants': ('Build full firmware', 'Build firmware without frontend resources'),
    'DAIKIN ESP32 full firmware': ('Test actual OTA handler, MessagePack wire and device sessions', 'Test ESP32 protocol and actual embedded page'),
    'DAIKIN ESP32 hub firmware': ('Test actual OTA handler, MessagePack wire and device sessions',
        'Check out pinned ESP-IDF NVS host-test sources', 'Test hub UDP, device bus and bounded storage',
        'Test production HAP value handling and byte output', 'Test locked hub LAN management frontend'),
    'Lightweight release checks': ('Test signed OTA vectors and Flash faults', 'Test OTA release tooling and Worker',
        'Test production TLS transport with real TLS and sanitizers'),
    'Binding and automation host scenarios': ('Test engines, binding runtime and WebUI syntax',
        'Test TMCP using real SDK workers and an isolated broker', 'Test local LAN codec and client lifecycle'),
    'Validate complete test evidence': ('Require all builds and current or reusable tests',),
}


class Rejected(ValueError):
    pass


EVIDENCE_ERRORS = (Rejected, KeyError, TypeError, AttributeError, OSError,
                   RuntimeError, UnicodeError, json.JSONDecodeError, zipfile.BadZipFile)


def require(value, reason):
    if not value:
        raise Rejected(reason)


def digest(body):
    return hashlib.sha256(body).hexdigest()


def fingerprint(root=ROOT):
    def git(*args):
        return subprocess.check_output(['git', '-C', str(root), *args], text=True).strip()
    frontend = json.loads((root / 'frontend.lock.json').read_bytes())
    protocol = json.loads((root / 'protocols.lock.json').read_bytes())
    require(frontend.get('schema') == protocol.get('schema') == 1 and
            re.fullmatch('[0-9a-f]{40}', frontend.get('commit', '')) and
            re.fullmatch('[0-9a-f]{64}', frontend.get('sha256', '')) and
            protocol.get('repository') == 'phicomm-io/iotcloud-community-protocols' and
            re.fullmatch('[0-9a-f]{40}', protocol.get('commit', '')), 'invalid dependency lock')
    return dict(source_commit=git('rev-parse', 'HEAD'), source_tree=git('rev-parse', 'HEAD^{tree}'),
        frontend_lock_sha256=digest((root / 'frontend.lock.json').read_bytes()),
        frontend_archive_sha256=frontend['sha256'], protocols_lock_sha256=digest((root / 'protocols.lock.json').read_bytes()),
        protocols_commit=protocol['commit'], workflow_sha256=digest((root / WORKFLOW).read_bytes()))


def gate_results(needs):
    require(set(needs) == set(NEEDS), 'incomplete validation job set')
    require(all(needs[job]['result'] == 'success' for job in NEEDS if job != 'feature-tests'), 'build or current release checks did not pass')
    reused = needs['frontend'].get('outputs', {}).get('reuse-heavy-validation') == 'true'
    require(needs['feature-tests']['result'] == ('skipped' if reused else 'success'), 'heavy tests did not pass or reuse was not verified')
    if reused:
        require(re.fullmatch('[1-9][0-9]*', needs['frontend']['outputs'].get('validation-run-id', '')), 'missing verified main run')
    return reused


def create(needs, context, expected):
    require(not gate_results(needs), 'reused validation cannot mint another full-test proof')
    require(context['event'] == 'push' and context['ref'] == 'refs/heads/main', 'only a full main push may mint evidence')
    require(type(context['run_id']) is int and context['run_id'] > 0 and
            type(context['run_attempt']) is int and context['run_attempt'] > 0, 'invalid run identity')
    return dict(schema=1, profile=PROFILE, repository=context['repository'], run_id=context['run_id'],
                run_attempt=context['run_attempt'], event='push', branch='main', fingerprint=expected,
                jobs={job: 'success' for job in NEEDS})


class SafeRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, msg, headers, url):
        require(urllib.parse.urlsplit(url).scheme == 'https', 'insecure artifact redirect')
        redirected = super().redirect_request(request, fp, code, msg, headers, url)
        # Artifact downloads redirect to storage. Never forward the API token.
        redirected.remove_header('Authorization')
        return redirected


class GitHub:
    def __init__(self, repository, token):
        require(re.fullmatch('[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', repository), 'invalid repository')
        self.base = 'https://api.github.com/repos/' + repository
        self.token = token
        self.opener = urllib.request.build_opener(SafeRedirect())

    def get(self, path, binary=False):
        require(path.startswith('/') and not path.startswith('//'), 'invalid API path')
        headers = {'Accept': 'application/vnd.github+json', 'User-Agent': 'phicomm-validation-evidence',
                   'X-GitHub-Api-Version': '2022-11-28'}
        if self.token:
            headers['Authorization'] = 'Bearer ' + self.token
        with self.opener.open(urllib.request.Request(self.base + path, headers=headers), timeout=15) as response:
            body = response.read((128 * 1024 if binary else 2 * 1024 * 1024) + 1)
        require(len(body) <= (128 * 1024 if binary else 2 * 1024 * 1024), 'oversized API evidence')
        return body if binary else json.loads(body)


def validate(run_id, expected, repository, api):
    run = api.get('/actions/runs/' + str(run_id))
    workflow = api.get('/actions/workflows/build.yml')
    require(run.get('id') == run_id and run.get('status') == 'completed' and run.get('conclusion') == 'success', 'main run is not completed successfully')
    require(run.get('event') == 'push' and run.get('head_branch') == 'main' and
            run.get('head_sha') == expected['source_commit'], 'run is not an exact-source main push')
    require(run.get('repository', {}).get('full_name') == repository and
            run.get('head_repository', {}).get('full_name') == repository and
            run.get('workflow_id') == workflow.get('id') and workflow.get('path') == WORKFLOW and
            run.get('path') == WORKFLOW, 'run repository or workflow differs')
    attempt = run.get('run_attempt')
    require(type(attempt) is int and attempt > 0, 'invalid main run attempt')
    listing = api.get(f'/actions/runs/{run_id}/attempts/{attempt}/jobs?per_page=100')
    jobs = listing['jobs']
    require(listing['total_count'] == len(jobs) <= 100, 'incomplete main job listing')
    for name, steps in JOBS.items():
        matches = [job for job in jobs if job['name'] == name]
        require(len(matches) == 1 and matches[0]['status'] == 'completed' and matches[0]['conclusion'] == 'success', 'required full-validation job missing or unsuccessful: ' + name)
        for name in steps:
            matching = [step for step in matches[0]['steps'] if step['name'] == name]
            require(len(matching) == 1 and matching[0]['conclusion'] == 'success', 'required heavy/current validation step did not execute: ' + name)
    listing = api.get(f'/actions/runs/{run_id}/artifacts?per_page=100')
    artifacts = listing['artifacts']
    require(listing['total_count'] == len(artifacts) <= 100, 'incomplete artifact listing')
    matching = [artifact for artifact in artifacts if artifact['name'] == ARTIFACT]
    require(len(matching) == 1 and not matching[0]['expired'], 'full validation evidence missing, duplicated or expired')
    artifact = matching[0]
    require(artifact.get('workflow_run', {}).get('id') == run_id and
            artifact['workflow_run'].get('head_sha') == expected['source_commit'], 'artifact run/source differs')
    body = api.get(f'/actions/artifacts/{artifact["id"]}/zip', binary=True)
    if artifact.get('digest'):
        require(artifact['digest'] == 'sha256:' + digest(body), 'artifact digest differs')
    with zipfile.ZipFile(io.BytesIO(body)) as zipped:
        require(zipped.namelist() == ['validation.json'] and zipped.getinfo('validation.json').file_size <= 65536, 'invalid validation artifact layout')
        proof = json.loads(zipped.read('validation.json'))
    require(type(proof) is dict and all(type(proof.get(key)) is int for key in ('schema', 'run_id', 'run_attempt')),
            'invalid validation proof types')
    require(proof == dict(schema=1, profile=PROFILE, repository=repository, run_id=run_id,
        run_attempt=attempt, event='push', branch='main', fingerprint=expected,
        jobs={job: 'success' for job in NEEDS}), 'validation source, locks, profile or run binding differs')
    return dict(reused=True, run_id=run_id, run_attempt=attempt, fingerprint=expected,
                artifact_id=artifact['id'], evidence_sha256=digest(body))


def resolve(expected, repository, ref, explicit, api):
    if explicit:
        require(re.fullmatch('[1-9][0-9]*', explicit), 'reuse run must be a positive numeric ID')
        require(ref.startswith('refs/tags/'), 'explicit validation reuse requires an actual tag')
    if not ref.startswith('refs/tags/'):
        return dict(reused=False, reason='full tests required for branch and PR runs')
    try:
        if explicit:
            return validate(int(explicit), expected, repository, api)
        query = urllib.parse.urlencode(dict(event='push', branch='main', status='success',
            head_sha=expected['source_commit'], per_page=5))
        candidates = api.get('/actions/workflows/build.yml/runs?' + query)['workflow_runs']
        for run in candidates:
            if run.get('head_sha') != expected['source_commit']:
                continue
            try:
                return validate(run['id'], expected, repository, api)
            except EVIDENCE_ERRORS:
                continue
        return dict(reused=False, reason='no complete exact-source main validation evidence')
    except EVIDENCE_ERRORS as error:
        if explicit:
            raise Rejected('explicit validation run rejected: ' + type(error).__name__) from None
        return dict(reused=False, reason='main validation evidence unavailable; full tests required')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=('resolve', 'gate', 'create'))
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.mode == 'gate':
        gate_results(json.loads(os.environ['VALIDATION_NEEDS']))
        print('All builds and current or exact-source heavy tests passed')
        return
    expected = fingerprint()
    if args.mode == 'create':
        result = create(json.loads(os.environ['VALIDATION_NEEDS']), dict(repository=os.environ['GITHUB_REPOSITORY'],
            run_id=int(os.environ['GITHUB_RUN_ID']), run_attempt=int(os.environ['GITHUB_RUN_ATTEMPT']),
            event=os.environ['GITHUB_EVENT_NAME'], ref=os.environ['GITHUB_REF']), expected)
    else:
        repository = os.environ['GITHUB_REPOSITORY']
        result = resolve(expected, repository, os.environ['GITHUB_REF'], os.environ.get('REUSE_VALIDATION_RUN', ''),
                         GitHub(repository, os.environ.get('GH_TOKEN', '')))
        with open(os.environ['GITHUB_OUTPUT'], 'a') as output:
            output.write('reuse=' + str(result['reused']).lower() + '\n')
            output.write('run-id=' + str(result.get('run_id', '')) + '\n')
        print('Heavy validation reuse:', result.get('run_id', result.get('reason')))
    require(args.output is not None, '--output is required')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, sort_keys=True, indent=2) + '\n')


if __name__ == '__main__':
    try:
        main()
    except Rejected as error:
        raise SystemExit(str(error)) from None
