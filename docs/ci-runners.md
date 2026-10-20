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
