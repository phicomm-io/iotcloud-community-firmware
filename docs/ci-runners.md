# Optional build runners

Pushes and pull requests continue to run on GitHub-hosted Ubuntu 22.04. Manual
**Build community firmware** runs default to `runner=github-hosted` as well.

To opt in, choose a branch in **Run workflow** and select `runner=self-hosted`.
The six firmware builds (three models, two variants) and `feature-tests` then
require all labels: `self-hosted`, `linux`, `x64`, `phicomm-build`. A missing or
busy matching runner leaves jobs queued; there is no automatic fallback. Select
`github-hosted` on a new manual run to use GitHub infrastructure again.

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
