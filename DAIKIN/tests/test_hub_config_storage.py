"""Production config/Preferences with IDF 4.4.7 NVS and a 16 KiB NOR model.

Set DAIKIN_NVS_IDF_SOURCE to an unpacked official esp-idf source tree at commit
38eeba213aa695aabfd6d89aa9f5078dbe5a94c3 (v4.4.7). The test uses
unchanged Storage/Page/PageManager/hash/CRC sources; it never accesses a device.
This optional integration test skips if that dependency is absent; no SDK is
vendored or fetched by the test. See review/daikin-hub-config-storage-review.md
in the surrounding workspace for dependency preparation and evidence.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
PIO = Path(os.environ.get('PLATFORMIO_CORE_DIR', str(Path.home() / '.platformio')))
ARDUINO = Path(os.environ.get('DAIKIN_ARDUINO_SOURCE',
    str(PIO / 'packages/framework-arduinoespressif32')))


def section(path, start, end):
    return start + path.read_text().split(start, 1)[1].split(end, 1)[0]


class HubConfigStorage(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        configured = os.environ.get('DAIKIN_NVS_IDF_SOURCE')
        if not configured:
            raise unittest.SkipTest('Set DAIKIN_NVS_IDF_SOURCE to official esp-idf v4.4.7 sources')
        sdk = Path(configured)
        nvs = sdk / 'components/nvs_flash'
        if not (nvs / 'src/nvs_storage.cpp').is_file():
            raise RuntimeError('DAIKIN_NVS_IDF_SOURCE is not an ESP-IDF source tree')
        version = (sdk / 'components/esp_common/include/esp_idf_version.h').read_text()
        for name, value in [('MAJOR', '4'), ('MINOR', '4'), ('PATCH', '7')]:
            import re
            if not re.search(r'#define\s+ESP_IDF_VERSION_' + name + r'\s+' + value + r'\b', version):
                raise RuntimeError('These regressions require the production IDF 4.4.7 version')
        cls.temp = tempfile.TemporaryDirectory(prefix='daikin-config-nvs-')
        temp = Path(cls.temp.name)
        main = ROOT / 'DAIKIN/src/main.cpp'
        (temp / 'load.inc').write_text(section(main, 'static String loadConfigText()', 'static bool due('))
        (temp / 'save.inc').write_text(section(main, 'static bool saveConfig(', 'static void startAccessPoint('))
        prefs = ARDUINO / 'libraries/Preferences/src/Preferences.cpp'
        methods = [
            ('size_t Preferences::putString(const char* key, const char* value)', 'PreferenceType Preferences::getType('),
            ('PreferenceType Preferences::getType(', '/*\n * Get a key value'),
            ('String Preferences::getString(const char* key, const String defaultValue)', 'size_t Preferences::freeEntries('),
        ]
        (temp / 'preferences.inc').write_text('\n'.join(section(prefs, a, b) for a, b in methods))
        # Only board I/O and log configuration are replaced. NVS logic is unmodified.
        (temp / 'sdkconfig.h').write_text(
            '#define CONFIG_SPI_FLASH_USE_LEGACY_IMPL 1\n'
            '#define CONFIG_LOG_MAXIMUM_LEVEL 0\n'
            '#define CONFIG_LOG_DEFAULT_LEVEL 0\n'
            '#define CONFIG_LOG_TIMESTAMP_SOURCE_RTOS 1\n'
            '#define CONFIG_IDF_TARGET_LINUX 1\n'
            '#define CONFIG_NVS_ASSERT_ERROR_CHECK 1\n')
        (temp / 'esp_spi_flash.h').write_text(
            '#pragma once\n#include "esp_err.h"\n#define SPI_FLASH_SEC_SIZE 4096\n'
            '#define ESP_ERR_FLASH_OP_FAIL (ESP_ERR_FLASH_BASE+1)\n'
            '#define ESP_ERR_FLASH_OP_TIMEOUT (ESP_ERR_FLASH_BASE+2)\n')
        codec = ROOT / 'DAIKIN/.pio/libdeps/bry88ab-hub/ArduinoJson/src'
        includes = [temp, ROOT / 'DAIKIN/include', codec, nvs / 'src', nvs / 'include',
                    nvs / 'private_include', sdk / 'components/esp_rom/include',
                    sdk / 'components/esp_rom/include/linux', sdk / 'components/log/include',
                    sdk / 'components/esp_common/include']
        sources = [ROOT / 'DAIKIN/tests/hub/config_storage.cpp'] + [nvs / 'src' / f
            for f in ('nvs_types.cpp', 'nvs_page.cpp', 'nvs_pagemanager.cpp',
                      'nvs_storage.cpp', 'nvs_item_hash_list.cpp')]
        sources += [sdk / 'components/esp_rom/linux/esp_rom_crc.c']
        cls.binary = {}
        for variant in ('hub', 'full'):
            binary = temp / variant
            command = ['c++', '-std=c++11', '-g', '-O1', '-DLINUX_TARGET', '-DNO_DEBUG_STORAGE',
                       '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie']
            if variant == 'hub':
                command.append('-DIOTCLOUD_HUB=1')
            command += ['-I' + str(p) for p in includes] + [str(p) for p in sources]
            command += ['-o', str(binary)]
            subprocess.run(command, check=True)
            cls.binary[variant] = binary

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def scenario(self, name, variant='hub'):
        # A real power loss discards the whole heap. Throwing from the NOR shim
        # instead abandons IDF's local non-RAII temporary list; that host-only
        # artifact must not be mistaken for an ordinary-operation firmware leak.
        leaks = '0' if name == 'power' else '1'
        subprocess.run([str(self.binary[variant]), name], check=True, timeout=90,
                       env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=' + leaks})

    def test_real_idf_same_key_type_collision_and_stale_read(self):
        self.scenario('collision')

    def test_new_key_migrates_without_erasing_legacy_or_wifi(self):
        self.scenario('migration')

    def test_present_invalid_hub_key_never_falls_back(self):
        self.scenario('invalid')

    def test_first_commit_and_update_survive_every_nor_write_boundary(self):
        self.scenario('power')

    def test_twelve_devices_and_eight_mappings_with_reboots_and_pool_moves(self):
        self.scenario('budget')

    def test_failed_or_over_limit_save_keeps_live_and_durable_state(self):
        self.scenario('failure')

    def test_legacy_blob_fallback_and_full_variant_compatibility(self):
        self.scenario('fallback')
        self.scenario('fallback', 'full')

    def test_full_string_save_limit_failure_reboot_and_pool_transfer(self):
        self.scenario('full', 'full')

    def test_partial_string_serialization_and_version_oom_never_touch_nvs(self):
        self.scenario('short')
        self.scenario('short', 'full')


if __name__ == '__main__':
    unittest.main()
