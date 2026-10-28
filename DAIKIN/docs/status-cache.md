# Business status cache

The application loop calls `pollBusinessStatusCache(now)` before processing requests. Exact typed source keys detect sensor sample/validity changes, CO2 warmup/cooldown and calibration transitions, network state, RSSI, indicator enablement, recovery state and button state. Successful configuration persistence must call `invalidateBusinessStatusCache()`; a status read following a write refreshes dirty data synchronously. No JSON serialization is used to compare keys.

A stable source returns the existing complete business snapshot and MP byte buffers. MQTT state and HTTP status reuse those buffers. Command envelopes with request IDs still serialize their own wrapper. Sensor `age_ms` is the age at `snapshot_uptime_ms`; clients extrapolate from that timestamp. Timer boundaries refresh even without new samples, including the 20-second sensor validity boundary.

The cache owns a 4096-byte JSON pool and two independent wire buffers (raw business state and HTTP envelope); rebuild temporarily requires another 4096-byte envelope pool and replacement strings. It runs only on the application loop. SDK callbacks must never access these buffers. Failed rebuild invalidates readiness before allocation so stale valid readings cannot be served.

Host regression uses production state/cache code, ArduinoJson 6.21.5 and the real ESP32 String implementation under ASAN/UBSAN. It checks stable-byte reuse, expiry, sample changes, configuration invalidation, timers and numeric diagnostic presence/absence. This is not a device performance result or an OOM injection test. The old embedded-broker publication allocation scenarios no longer represent the product transport and are removed from this runner.
