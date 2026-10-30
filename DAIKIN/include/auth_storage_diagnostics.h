#pragma once
#include <stddef.h>
// Stable wire error stages; append new values, never renumber.
enum class AuthStorageError : unsigned { ok=0,restart_pending=1,invalid_input=2,too_large=3,encode_failed=4,old_read_failed=5,allocation_failed=6,stats_failed=7,insufficient_entries=8,write_failed=9,verify_failed=10 };
struct AuthStorageDiagnostics {
  bool attempted=false,sizes=false,stats=false,required=false;
  size_t freeEntries=0,usedEntries=0,totalEntries=0,requiredEntries=0,oldBytes=0,newBytes=0;
  AuthStorageError error=AuthStorageError::ok;
};
