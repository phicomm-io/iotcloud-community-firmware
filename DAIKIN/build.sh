#!/bin/sh
set -eu
cd "$(dirname "$0")"
"${PIO:-pio}" run -e bry88ab
python3 package.py
