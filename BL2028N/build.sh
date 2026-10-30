#!/usr/bin/env bash
set -euo pipefail
source "${XDG_CACHE_HOME:-$HOME/.cache}/phicomm/env.sh" gcc5
exec python3 "$(dirname "$0")/build.py" "$@"
