STATUS_STRING("reset_reason",s->reset_name);
STATUS_INT("reset_reason_raw",s->reset_raw);
STATUS_BOOL("watchdog_enabled",s->enabled);
STATUS_STRING("watchdog_state",s->failed ? "fault" : s->ready ? "healthy" : "starting");
STATUS_STRING("watchdog_fault",s->fault);
