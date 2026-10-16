# A1/M1 runtime fleet

These are validation tools, not firmware services. Fourteen native processes run
production feature, group, engine, service, automation and MQTT workers, with the
pinned SDK MQTT client. POSIX threads/networking and RAM configuration replace
RTOS, Flash and physical peripherals. Host success does not establish MCU stack,
heap, UART, display, fan or optical correctness. The separate `tests/device/`
runners collect actual A1/M1 evidence.

Build SDK workspaces first, then:

```sh
SANITIZE=1 bash tests/simulator/build.sh
ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 python3 tests/simulator/fleet.py \
  --user-file build/evidence/private/a1-original.json
python3 tests/simulator/scenarios.py
python3 tests/simulator/control_scenarios.py
python3 tests/simulator/group_scenarios.py
python3 tests/simulator/fault_scenarios.py
```

`--user-file` reads the MQTT user from the privately saved original configuration.
Never publish that file or credentials. The defaults select the explicitly
provisioned anonymous **test** broker at `10.96.210.226:32768`. Configuration is
in-memory and changes do not survive process termination. IDs/IPs are fixed test
identities; the control API listens only on loopback `32800`.

Start `proxy.py` separately for seven UDP relays on `127.30.2.1` through `.7`, with
loopback control port `32801`. `fault_scenarios.py` rotates offline, delay, drop,
duplicate, reorder, wrong token, stale age and invalid sensor payload faults at
least twenty times each, checks that packets traversed each injection, then
requires fresh LAN recovery within thirty seconds. `control_scenarios.py` checks
actual fixed/linear/PID behavior and stale/invalid sample exclusion.

`group_scenarios.py` configures four A1 members and exercises control/config fanout
and twenty coordinator pause/takeover/rejoin cycles. Do not query a SIGSTOP-paused
node through the fleet API, and do not attach a debugger that changes SIGSTOP
semantics. Native request IDs prevent late replies from being mistaken for a
later command.

After independent normal-load device checks and explicit DUT/broker authorization,
`python3 tests/simulator/physical_matrix.py setup` adds A1 `b0f893303134` at
`10.117.152.111` and M1
`b0f8932864b6` at `10.117.152.110`, producing eight nodes per model and sixty-four
bindings. Host loopback addresses are unreachable from the DUT: those edges force
MQTT; the physical pair uses LAN. `physical_matrix.py broker-faults` stops/starts
only the named dedicated Mosquitto container twenty times, checks runtime APIs
and uptime during the fault, and requires MQTT and sensor availability recovery.
Run `python3 tests/simulator/physical_matrix.py broker-faults` only after separate
authorization. Never point fault tests at a user's normal broker.

After all authorized binding and broker changes have finished, `fleet_soak.py`
samples the already-running 14-node host fleet every five seconds for four hours
by default. Use one of these explicit topologies:

```sh
python3 tests/simulator/fleet_soak.py --expected-peers 7
python3 tests/simulator/fleet_soak.py --physical-matrix
```

The physical mode requires seven native fresh LAN peers and the physical M1 as a
fresh MQTT peer; sensor values are checked for validity and range, not against a
fixed host value. The runner checks fixed process identities/PIDs, continuous
uptime, MQTT state and stable MQTT error counters. It writes an NDJSON health log
and summary under `build/evidence/fleet/`; it does not measure MCU heap or stack.

The parent session owns shared bindings, grouping and broker faults. Independent
DUT runners must be paused for those mutations. Restore original bindings,
linkage and broker settings from private snapshots afterward; preserve original
credentials, schedules, button mappings and current user settings. No factory
reset, Flash erase or power interruption during OTA is part of this suite.

Evidence is written beneath ignored `build/evidence/`; malformed response bodies
and original configurations remain private. Record source/binary/signed-image
hashes and exact durations separately. An interrupted load stage is incomplete,
even if every request so far passed. LeakSanitizer is disabled for intentionally
long-running fleet processes; standalone allocation/lifetime regression tests
run with leak checks enabled. Do not treat the fleet as a leak-proof result.
