#!/bin/sh
set -eu
cd "$(dirname "$0")"
[ "${PHICOMM_FIRMWARE_VARIANT:-full}" = full ] || { echo 'DAIKIN provides only the full firmware' >&2; exit 1; }
"${PIO:-pio}" run -e bry88ab
python3 package.py
