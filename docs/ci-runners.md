# Optional build runners

The repository variable `PHICOMM_CI_RUNNER` selects the default build/test runner.
Unset (or any value other than `self-hosted`) means GitHub-hosted Ubuntu 22.04.
Set it to `self-hosted` to route branch/tag pushes and same-repository pull requests
to the optional runner. Clear it to restore GitHub-hosted scheduling.

Manual **Build community firmware** runs default to `runner=auto`, following that
variable. Explicit `github-hosted` or `self-hosted` overrides the variable.

| Event/ref | Runner selection |
| --- | --- |
| Branch push / same-repository PR | Repository variable, GitHub-hosted if unset |
| Fork PR | Always GitHub-hosted |
| Manual branch/tag, `auto` | Repository variable, GitHub-hosted if unset |
| Manual branch/tag, explicit choice | Selected runner |
| Tag push | Repository variable, GitHub-hosted if unset |

Runner selection applies to frontend validation, the six firmware builds
(three models, two variants), `feature-tests`, signing and publication, requiring all labels: `self-hosted`, `linux`,
`x64`, `phicomm-build`. A missing or busy matching runner leaves jobs queued;
there is no automatic fallback. Choose `github-hosted` on a new manual run or
clear the repository variable to use GitHub infrastructure again.

The self-hosted runner must provide an Ubuntu 22.04-compatible build environment,
passwordless `sudo` for the existing dependency installation, and the tooling
used by the unchanged workflow steps. Containerizing the runner itself is an
infrastructure choice; this workflow does not add a job container. Use a clean
runner workspace between jobs/runs, particularly for SDK and compiler outputs.

Signing and publication explicitly require GitHub CLI. If absent,
`tools/ci/ensure_gh.sh` installs the checksum-pinned Linux x64 release in
`/usr/local/bin` using passwordless `sudo`. The manual **Prepare self-hosted
release tools** workflow can prepare an existing runner before retrying an older
tag whose workflow did not include this dependency check. It uses a read-only
token and no production environment secrets.

Tag runs follow the same runner selection, including signing and publication.
Signing and publication retain their tag-only conditions, approved-main source
check, production environment and release concurrency group. Production secrets
are available only to those environment-scoped jobs. The signing key is written
to a restricted temporary file and removed when signing exits. Use only a trusted
runner for production tags. Fork PRs always run on GitHub-hosted infrastructure.
Manual branch runs only build/test and upload artifacts; they do not sign or
publish a release. Build/test jobs retain a read-only GitHub token.

GitHub requires the dispatch workflow to exist on the default branch before it
can be run manually. This document does not imply that the optional runner is
registered or that a self-hosted build has passed.

References: [runner labels and expressions](https://docs.github.com/en/actions/how-tos/write-workflows/choose-where-workflows-run/choose-the-runner-for-a-job),
[dispatch choice inputs](https://docs.github.com/en/actions/reference/workflows-and-actions/workflow-syntax#onworkflow_dispatchinputs),
[expression JSON conversion](https://docs.github.com/en/actions/reference/workflows-and-actions/expressions#fromjson).

## Model matrix and caches

Three parallel jobs (`tc1`, `m1`, `a1`) each build both variants in sequence.
This preserves all six artifact names and binaries while sharing setup within a
model. Compared with six parallel jobs, GitHub-hosted cold builds may have a
longer critical path, but need three SDK/tool preparations instead of six. A
single self-hosted runner also avoids repeating setup between each variant.
The second variant is attempted after a first-variant failure when preparation
succeeded and the run was not cancelled; the first failure still fails the job.
TC1 host regressions run once. Signing and host scenarios require the complete
firmware matrix to succeed.

Only the pinned raw bare SDK repository is cached, keyed by
`common/mico-sdk.json`. Product-patched SDK checkouts are separate per model;
the two variants reuse the same model's idempotent patch stack but have separate
build workspaces. Patched SDKs, objects, generated files and MiCoder absolute
symlinks are never cached. Existing product scripts recreate MiCoder links.

GCC 5.4.1 and installed Python 2.7.18 share `build/ci-tools` within each job.
Their cache key includes architecture and the absolute checkout path because
Python's configured prefix is not assumed relocatable. Restored GCC version and
Python version/prefix are checked; incompatible installations are discarded so
existing product scripts rebuild them. Source trees and Python build logs are
not cached. Dependencies explicitly include `libc6-i386`: the pinned GCC Linux
executables require the 32-bit loader even on an x64 runner.

Both firmware and host-test jobs explicitly point `ARM_CC` at the shared pinned
compiler. TC1 regressions and host tests require compiler version/target, hard
and soft ABI compile probes, and QEMU before running tests, so a missing shared
compiler cannot silently remove ARM coverage. Host tests restore the same tool
cache and fetch the checksum-pinned compiler if the cache is unavailable.
Earlier optimized runs produced six firmware artifacts but skipped ARM suites
because they still looked under `build/tc1-tools`; those runs are not ARM test
acceptance evidence.

The early feature-test SDK setup also applies the existing TC1 HTTP overlay
stack when `TC1/build.sh` still invokes that stack. Main's HTTP admission and
recovery tests inspect those patched functions directly. Shared-HTTP builds that
no longer invoke the legacy stack leave that SDK code untouched; leftover patch
files alone do not select the legacy preparation path.
