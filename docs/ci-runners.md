# Optional build runners

The repository variable `PHICOMM_CI_RUNNER` selects the default build/test runner.
Unset (or any value other than `self-hosted`) means GitHub-hosted Ubuntu 22.04.
Set it to `self-hosted` to route branch pushes and same-repository pull requests
to the optional runner. Clear it to restore GitHub-hosted scheduling.

Manual **Build community firmware** runs default to `runner=auto`, following that
variable. Explicit `github-hosted` or `self-hosted` overrides the variable.

| Event/ref | Runner selection |
| --- | --- |
| Branch push / same-repository PR | Repository variable, GitHub-hosted if unset |
| Fork PR | Always GitHub-hosted |
| Manual branch, `auto` | Repository variable, GitHub-hosted if unset |
| Manual branch, explicit choice | Selected runner |
| Any tag | Always GitHub-hosted |

Self-hosted selection applies to the six firmware builds (three models, two
variants) and `feature-tests`, requiring all labels: `self-hosted`, `linux`,
`x64`, `phicomm-build`. A missing or busy matching runner leaves jobs queued;
there is no automatic fallback. Choose `github-hosted` on a new manual run or
clear the repository variable to use GitHub infrastructure again.

The self-hosted runner must provide an Ubuntu 22.04-compatible build environment,
passwordless `sudo` for the existing dependency installation, and the tooling
used by the unchanged workflow steps. Containerizing the runner itself is an
infrastructure choice; this workflow does not add a job container. Use a clean
runner workspace between jobs/runs, particularly for SDK and compiler outputs.

Tag runs always use GitHub-hosted runners, even when manually requested with
`runner=self-hosted`. Signing and publication retain their existing tag-only
conditions, GitHub-hosted runners, protected environment, and secrets. Manual
branch runs only build/test and upload the existing build artifacts; they do not
sign or publish a release. No signing or deployment secret is added to the
self-hosted jobs; they retain the workflow's read-only GitHub token.

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
