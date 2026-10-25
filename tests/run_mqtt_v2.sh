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
bash tests/simulator/build.sh
if [[ " ${models[*]} " == *" A1 "* ]]; then
  "${MQTT_TEST_PYTHON:-python3}" tests/test_a1_filter_mqtt.py "$port"
else
  echo 'NOT_RUN A1 filter MQTT: A1 not selected'
fi
if [[ " ${models[*]} " == *" M1 "* ]]; then
  "${MQTT_TEST_PYTHON:-python3}" tests/test_m1_co2_mqtt.py "$port"
  "${MQTT_TEST_PYTHON:-python3}" tests/test_m1_external_co2_mqtt.py "$port"
  "${MQTT_TEST_PYTHON:-python3}" tests/test_m1_dual_display_mqtt.py "$port"
  "${MQTT_TEST_PYTHON:-python3}" tests/test_m1_pressure_mqtt.py "$port"
  if [[ " ${models[*]} " == *" A1 "* ]]; then
    "${MQTT_TEST_PYTHON:-python3}" tests/test_sensor_session_mqtt.py "$port"
    "${MQTT_TEST_PYTHON:-python3}" tests/test_a1_auto_mqtt.py "$port"
    "${MQTT_TEST_PYTHON:-python3}" tests/test_a1_co2_mqtt.py "$port"
  else
    echo 'PAUSED A1 automatic MQTT: requires A1 and M1; A1 not selected'
    echo 'PAUSED A1/M1 CO2 linkage MQTT: requires A1 and M1; A1 not selected'
  fi
  "${MQTT_TEST_PYTHON:-python3}" tests/test_resource_mqtt.py "$port"
else
  echo 'PAUSED A1 automatic MQTT: scenario starts an M1 host simulator'
  echo 'PAUSED M1 CO2 MQTT: M1 testing is paused'
  echo 'PAUSED A1/M1 CO2 linkage MQTT: scenario starts an M1 host simulator'
  echo 'PAUSED M1 resource MQTT: M1 testing is paused'
fi
