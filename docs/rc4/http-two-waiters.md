# Two waiting HTTP requests

A1, M1 and TC1 share a maximum of two READY requests behind one active
application exchange. Total connection slots remain A1=3, M1=4 and TC1=4.
The one-second READY deadline, single response/body owner, OTA admission of
two connections, and application/local-input priorities are unchanged.
Waiting requests do not allocate an application request or response buffer.
The status diagnostic `http_accept_queue_capacity` now reports 2.

The scheduler selects the earliest waiting deadline using wrap-safe time
comparison, retains the selected waiter on equal deadlines, and admits an
existing waiter before a newly completed header even when the active response
finishes in the same I/O turn. A third waiter receives busy when a connection
slot is available; at the total connection limit, acceptance remains bounded.

Host verification uses the production core/parser at both three and four
connection slots under ASan/UBSan: bounded waiting, same-turn arrivals,
equal-deadline ordering, clock wrap, expiry, OTA cap reduction, cooperative
shutdown and recovery. Product HTTP callbacks cover all three models.

Device campaign: snapshot identity/config/backup first; install the exact
main-derived model/variant artifact; verify reboot identity, configuration,
Wi-Fi/MQTT/HA health, queue capacity and OTA idle. Compare baseline and candidate
under the same HTTP/MQTT/HA load, including three settings-tab changes within
one second. Record request and operation P95, busy count/rate, same-action
recovery time, network failures, memory and reset counters separately. Physical
button behavior requires physical evidence, not a host scheduler assertion.

Parallel simulated-device campaigns cover cloud legacy JSON/new MessagePack
coexistence, discovery upgrade/downgrade, state/command/result, account
isolation, reconnect and malformed messages; hub tests exercise actual
discovery, control/confirmation, forwarding and pairing paths. UDP peers use
the new binary protocol together, without legacy JSON fallback.
