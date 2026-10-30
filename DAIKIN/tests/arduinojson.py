"""Locate the pinned codec installed by either DAIKIN build variant."""
import os
from pathlib import Path


def codec_source():
    root = Path(__file__).resolve().parents[1]
    override = os.environ.get('DAIKIN_ARDUINOJSON_SOURCE')
    candidates = ([Path(override)] if override else
                  [root/'.pio/libdeps'/name/'ArduinoJson/src'
                   for name in ('bry88ab', 'bry88ab-hub')])
    for path in candidates:
        if (path/'ArduinoJson.h').is_file():
            return path
    raise RuntimeError('Build a DAIKIN target or set DAIKIN_ARDUINOJSON_SOURCE '
                       'to the pinned ArduinoJson source')
