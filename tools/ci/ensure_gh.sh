#!/usr/bin/env bash
# GitHub-hosted images include gh; self-hosted production jobs must prepare it.
set -euo pipefail

if command -v gh >/dev/null 2>&1; then
    gh --version
    exit 0
fi

test "$(uname -s):$(uname -m)" = Linux:x86_64
version=2.102.1
archive="gh_${version}_linux_amd64.tar.gz"
temporary=$(mktemp -d "${RUNNER_TEMP:-${TMPDIR:-/tmp}}/phicomm-gh.XXXXXX")
trap 'rm -rf "$temporary"' EXIT
curl -fsSL --retry 3 --connect-timeout 20 --max-time 600 \
    "https://github.com/cli/cli/releases/download/v${version}/${archive}" \
    -o "$temporary/$archive"
printf '%s  %s\n' 77b9e297e94c50336823d0f84de7ef1be60cba704601fa0d98f129f47a94987b \
    "$temporary/$archive" | sha256sum -c -
tar -xzf "$temporary/$archive" -C "$temporary" "gh_${version}_linux_amd64/bin/gh"
sudo install -m 755 "$temporary/gh_${version}_linux_amd64/bin/gh" /usr/local/bin/gh
gh --version
