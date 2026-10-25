/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#ifndef IOTCLOUD_HOME_STORAGE_HOST_TEST
#include <esp_flash.h>
#include <esp_partition.h>
#include <nvs_flash.h>
#endif

// Reserve only the unused tail of two known 4 MiB dual-OTA layouts. Installed
// partition tables remain unchanged. A separate marker prevents an unknown
// tail from being initialized or erased as if it were our pairing storage.
namespace iotcloud {
class HomeStorage {
public:
  static constexpr uint32_t Start = 0x3d0000, Length = 0x30000;
  esp_partition_t partition = {};
  bool initialized = false;
  bool owned = false, resetPending = false;
  bool open(bool claim) {
    if (resetPending) return false;
    if (initialized)
      return true;
    owned = false;
    uint32_t flashSize = 0;
    if (esp_flash_get_size(nullptr, &flashSize) != ESP_OK || flashSize != 0x400000)
      return false;
    auto a = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                     ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
    auto b = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                     ESP_PARTITION_SUBTYPE_APP_OTA_1, nullptr);
    if (!a || !b || !esp_flash_default_chip ||
        a->flash_chip != esp_flash_default_chip || b->flash_chip != esp_flash_default_chip ||
        a->encrypted || b->encrypted || a->address != 0x10000)
      return false;
    const bool wired = a->size == 0x1e0000 && b->address == 0x1f0000 && b->size == 0x1e0000;
    const bool stock = a->size == 0x180000 && b->address == 0x190000 && b->size == 0x180000;
    if (!wired && !stock) return false;
    // Validate the complete installed table, including data partitions. A
    // supported OTA pair does not authorize overlapping or external storage.
    auto it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, nullptr);
    if (!it) return false;
    for (; it;) {
      auto p = esp_partition_get(it);
      if (!p || p->flash_chip != esp_flash_default_chip || p->encrypted ||
          !p->size || p->address < 0x9000 || (p->address & 0xfff) || (p->size & 0xfff) ||
          p->address > flashSize || p->size > flashSize - p->address ||
          (p->type == ESP_PARTITION_TYPE_APP && p != a && p != b) ||
          (p->address < Start + Length && uint64_t(p->address) + p->size > Start)) {
        esp_partition_iterator_release(it);
        return false;
      }
      // Iterators allocate only their small cursor; compare without copying
      // the partition table or assuming that its entries are address-sorted.
      auto other = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, nullptr);
      if (!other) {
        esp_partition_iterator_release(it);
        return false;
      }
      for (; other;) {
        auto q = esp_partition_get(other);
        if (!q || (q != p && p->address < uint64_t(q->address) + q->size &&
                   q->address < uint64_t(p->address) + p->size)) {
          esp_partition_iterator_release(other);
          esp_partition_iterator_release(it);
          return false;
        }
        other = esp_partition_next(other);
      }
      it = esp_partition_next(it);
    }
    partition.flash_chip = a->flash_chip;
    partition.type = ESP_PARTITION_TYPE_DATA;
    partition.subtype = ESP_PARTITION_SUBTYPE_DATA_NVS;
    partition.address = Start + 0x1000;
    partition.size = Length - 0x1000;
    strcpy(partition.label, "iotc_hap");
    static const char marker[36] = "IoTCloud HAP NVS v1 3d1000 2f000";
    uint8_t data[256];
    if (esp_flash_read(partition.flash_chip, data, Start, sizeof(marker)) != ESP_OK)
      return false;
    if (memcmp(data, marker, sizeof(marker))) {
      if (!claim)
        return false;
      // No erase during initialization, including corrupt/unrecognized NVS.
      for (uint32_t off = 0; off < Length; off += sizeof(data)) {
        if (esp_flash_read(partition.flash_chip, data, Start + off, sizeof(data)) != ESP_OK)
          return false;
        for (auto byte : data)
          if (byte != 0xff)
            return false;
      }
      if (esp_flash_write(partition.flash_chip, marker, Start, sizeof(marker)) != ESP_OK)
        return false;
      if (esp_flash_read(partition.flash_chip, data, Start, sizeof(marker)) != ESP_OK ||
          memcmp(data, marker, sizeof(marker)))
        return false;
    } else {
      // No other format may share the marker sector, including future ones.
      for (uint32_t off = sizeof(marker); off < 0x1000;) {
        const auto count = std::min(sizeof(data), size_t(0x1000 - off));
        if (esp_flash_read(partition.flash_chip, data, Start + off, count) != ESP_OK)
          return false;
        for (size_t i = 0; i < count; ++i)
          if (data[i] != 0xff) return false;
        off += count;
      }
    }
    owned = true;
    initialized = nvs_flash_init_partition_ptr(&partition) == ESP_OK;
    return initialized;
  }
  bool reset() {
    if (resetPending) return false;
    // Recheck the ownership marker even if NVS had already been opened. A
    // failed prior init is not permission to erase subsequently changed data.
    initialized = false;
    if (!open(false) && !owned)
      return false;
    // Erase only recognized, owned storage, after explicit authenticated reset.
    // NVS handles may still exist; the caller restarts before further HAP work.
    initialized = false;
    resetPending = true;
    return esp_partition_erase_range(&partition, 0, partition.size) == ESP_OK;
  }
};
} // namespace iotcloud
