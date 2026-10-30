# TC1 features

## Six independent outlets and names

Name and switch each outlet separately or switch all at once. Names are labels and do not change protocol indices: `index` 0 means outlet 1, through 5 for outlet 6. TC1 hardware revision A1 and the A1 air purifier are different devices; their images are not interchangeable.

## Total power and energy

TC1 measures whole-strip power and energy, not voltage, current or power for each individual outlet. See [daily operation](tc1-guide#metering) for trends, peaks and report settings. Use an independent reference meter when adjusting the coefficient; a load's nameplate is not a calibration reference.

## Button mappings and child lock

Configure 1–30 clicks or a 1–4 second hold; holds of at least five seconds are reserved for recovery. There are no default custom mappings. Child lock restricts ordinary mapped actions, not recovery gestures. Local buttons take priority over remote commands. See [common functions → recovery](tc1-guide#recovery-gestures) for timing and feedback.

## Local power rules and remote readings

Development firmware provides up to eight power rules target one or all outlets and use total strip power. The `power` field can feed M1 over the same external MQTT broker. See [automation and remote display](tc1-automation) for device IDs, units and setup.
