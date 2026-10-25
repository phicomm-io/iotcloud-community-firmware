#!/bin/sh
set -eu
cd "$(dirname "$0")"
variant="${PHICOMM_FIRMWARE_VARIANT:-full}"
case "$variant" in
  full) target=bry88ab ;;
  hub) target=bry88ab-hub ;;
  *) echo "Unsupported DAIKIN variant: $variant" >&2; exit 1 ;;
esac
export PHICOMM_FIRMWARE_VARIANT="$variant"
"${PIO:-pio}" run -e "$target"
python3 package.py
