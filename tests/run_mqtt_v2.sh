#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
# Separate temporary broker; never connect tests to the user's configured broker.
output=$(mktemp -d)
container="phicomm-tmcp-test-$$"
trap 'docker rm -f "$container" >/dev/null 2>&1 || true; rm -rf "$output"' EXIT
printf 'listener 1883\nallow_anonymous true\npersistence false\n' > "$output/mosquitto.conf"
chmod 755 "$output"
chmod 644 "$output/mosquitto.conf"
docker run -d --name "$container" -p 127.0.0.1::1883 \
  -v "$output/mosquitto.conf:/mosquitto/config/mosquitto.conf:ro" eclipse-mosquitto:2.0.22 >/dev/null
port=$(docker port "$container" 1883/tcp | sed 's/.*://')
python3 - "$port" <<'PY'
import socket,sys,time
for attempt in range(50):
    try:
        with socket.create_connection(('127.0.0.1',int(sys.argv[1])),timeout=1):break
    except OSError:time.sleep(.1)
else:raise RuntimeError('isolated MQTT broker did not start')
PY
"${MQTT_TEST_PYTHON:-python3}" tests/test_mqtt_v2.py "$port"
bash tests/simulator/build.sh
"${MQTT_TEST_PYTHON:-python3}" tests/test_a1_filter_mqtt.py "$port"
