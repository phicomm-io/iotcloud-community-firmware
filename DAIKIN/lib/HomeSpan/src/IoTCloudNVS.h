/* SPDX-License-Identifier: MIT */
#pragma once
#include <cstddef>
#include <cstring>
#ifndef IOTCLOUD_HOME_STORAGE_HOST_TEST
#include <nvs.h>
#endif

namespace iotcloud {
// A failed read, write or commit latches this boot unhealthy. In particular a
// successful read-back after nvs_set_blob cannot substitute for nvs_commit.
class HomeNVS {
  bool ok = false;
public:
  enum Read { Error = -1, Missing = 0, Found = 1 };
  bool healthy() const { return ok; }
  void opened() { ok = true; }
  bool fail() { ok = false; return false; }
  Read blobLength(nvs_handle handle, const char *key, size_t &length,
                  size_t minimum, size_t maximum, size_t multiple = 1,
                  bool optional = false) {
    if (!ok) return Error;
    length = 0;
    auto result = nvs_get_blob(handle, key, nullptr, &length);
    if (optional && result == ESP_ERR_NVS_NOT_FOUND) return Missing;
    if (result != ESP_OK || length < minimum || length > maximum ||
        !multiple || length % multiple) { fail(); return Error; }
    return Found;
  }
  Read blob(nvs_handle handle, const char *key, void *data, size_t size,
            bool optional = false) {
    size_t length;
    const auto result = blobLength(handle, key, length, size, size, 1, optional);
    if (result != Found) return result;
    if (nvs_get_blob(handle, key, data, &length) != ESP_OK || length != size) {
      fail(); return Error;
    }
    return Found;
  }
  Read text(nvs_handle handle, const char *key, char *data, size_t capacity,
            bool optional = false) {
    if (!ok || !capacity) return Error;
    size_t length = 0;
    const auto result = nvs_get_str(handle, key, nullptr, &length);
    if (optional && result == ESP_ERR_NVS_NOT_FOUND) return Missing;
    if (result != ESP_OK || !length || length > capacity) { fail(); return Error; }
    const auto expected = length;
    if (nvs_get_str(handle, key, data, &length) != ESP_OK || length != expected ||
        data[length - 1] || std::strlen(data) + 1 != length) {
      fail(); return Error;
    }
    return Found;
  }
  bool writeBlob(nvs_handle handle, const char *key, const void *data, size_t size) {
    if (!ok) return false;
    if (nvs_set_blob(handle, key, data, size) != ESP_OK ||
        nvs_commit(handle) != ESP_OK) return fail();
    return true;
  }
  bool erase(nvs_handle handle, const char *key) {
    if (!ok) return false;
    const auto result = nvs_erase_key(handle, key);
    if ((result != ESP_OK && result != ESP_ERR_NVS_NOT_FOUND) ||
        nvs_commit(handle) != ESP_OK) return fail();
    return true;
  }
};
} // namespace iotcloud
