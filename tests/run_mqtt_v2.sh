#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
selected=$(python3 tests/host_models.py)
mapfile -t models <<< "$selected"
# Separate temporary broker; never connect tests to the user's configured broker.
output=$(mktemp -d)
container="phicomm-tmcp-test-$$"
cleanup() {
    result=$?
    if (( result != 0 )); then
        # Synthetic isolated broker only; preserve a bounded tail before removal.
        echo 'Isolated MQTT broker failure log (last 120 lines):' >&2
        docker logs --tail 120 "$container" >&2 2>&1 || true
    fi
    docker rm -fv "$container" >/dev/null 2>&1 || true
    rm -rf "$output"
    exit "$result"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
printf 'listener 1883\nallow_anonymous true\npersistence false\n' > "$output/mosquitto.conf"
chmod 755 "$output"
chmod 644 "$output/mosquitto.conf"
docker run -d --name "$container" -p 127.0.0.1::1883 \
  -v "$output/mosquitto.conf:/mosquitto/config/mosquitto.conf:ro" eclipse-mosquitto:2.0.22 >/dev/null
port=$(docker port "$container" 1883/tcp | sed 's/.*://')
python3 tests/mqtt_broker_ready.py "$port"
"${MQTT_TEST_PYTHON:-python3}" -m unittest tests.test_mqtt_observer tests.test_mqtt_broker_ready
"${MQTT_TEST_PYTHON:-python3}" tests/test_mqtt_v2.py "$port"
# The removed JSON-device protocol is not a supported native fallback. The
# native functional manifest runs in tests/run.sh before this broker gate;
# see the explicit historical-scenario mapping, including remaining gaps.
# Do not compile a JSON adapter to make obsolete wire assertions pass.
echo 'Native product, UDP/session, sensor, display and HA functional coverage: tests/coverage/rc4-native-migration.md'
