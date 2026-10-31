"""Production config/Preferences with IDF 4.4.7 NVS and 16/20 KiB NOR models.

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
from DAIKIN.tests.arduinojson import codec_source

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
        (temp / 'load.inc').write_text(section(main, 'static String loadLegacyConfigText(){', 'static bool due('))
        (temp / 'save.inc').write_text(section(main, 'static bool saveConfigTransaction(DynamicJsonDocument &candidate', 'static void startAccessPoint('))
        (temp/'mbedtls').mkdir()
        (temp/'mbedtls/sha256.h').write_text('#include <openssl/sha.h>\nstatic int mbedtls_sha256_ret(const unsigned char*p,size_t n,unsigned char*out,int){return SHA256(p,n,out)?0:-1;}\n')
        subprocess.run(['cc','-fsanitize=address,undefined','-fno-pie','-c',str(ROOT/'common/local_auth.c'),'-o',str(temp/'auth.o')],check=True)
        prefs = ARDUINO / 'libraries/Preferences/src/Preferences.cpp'
        methods = [
            ('bool Preferences::remove(const char * key)', '/*\n * Put a key value'),
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
        codec = codec_source()
        includes = [temp, ROOT/'common', ROOT / 'DAIKIN/include', codec, nvs / 'src', nvs / 'include',
                    nvs / 'private_include', sdk / 'components/esp_rom/include',
                    sdk / 'components/esp_rom/include/linux', sdk / 'components/log/include',
                    sdk / 'components/esp_common/include']
        sources = [ROOT / 'DAIKIN/tests/hub/auth_nvs.cpp'] + [nvs / 'src' / f
            for f in ('nvs_types.cpp', 'nvs_page.cpp', 'nvs_pagemanager.cpp',
                      'nvs_storage.cpp', 'nvs_item_hash_list.cpp')]
        sources += [sdk / 'components/esp_rom/linux/esp_rom_crc.c']
        cls.binary = {}
        for variant in ('hub', 'full', 'v2'):
            binary = temp / variant
            command = ['c++', '-std=c++11', '-g', '-O1', '-DLINUX_TARGET', '-DNO_DEBUG_STORAGE',
                       '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie']
            if variant in ('hub','v2'):
                command.append('-DIOTCLOUD_HUB=1')
            if variant=='v2': command.append('-DDAIKIN_FIXED_AUTH_V2=1')
            command += ['-I' + str(p) for p in includes] + [str(p) for p in sources]
            command += [str(temp/'auth.o'),'-lcrypto','-o', str(binary)]
            subprocess.run(command, check=True)
            cls.binary[variant] = binary
        if os.environ.get('DAIKIN_NVS_RC3_COMPARE'):
            baseline=subprocess.check_output(['git','show','6950338:DAIKIN/src/main.cpp'],cwd=ROOT,text=True)
            start=baseline.index('static bool saveConfig(');end=baseline.index('static void startAccessPoint(',start);(temp/'baseline-save.inc').write_text(baseline[start:end])
            binary=temp/'rc3';command=['c++','-std=c++11','-g','-O1','-DLINUX_TARGET','-DNO_DEBUG_STORAGE','-DIOTCLOUD_HUB=1','-DDAIKIN_BASELINE=1','-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie']+['-I'+str(p) for p in includes]+[str(p) for p in sources]+[str(temp/'auth.o'),'-lcrypto','-o',str(binary)];subprocess.run(command,check=True);cls.binary['rc3']=binary

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def scenario(self, name, variant='hub', pages=5, no_pairs=False):
        # A real power loss discards the whole heap. Throwing from the NOR shim
        # instead abandons IDF's local non-RAII temporary list; that host-only
        # artifact must not be mistaken for an ordinary-operation firmware leak.
        leaks = '0' if name in ('power','migration','live-reclaim-power','live-legacy','route-power') else '1'
        timeout = 300 if name in ('live-reclaim-power', 'live-legacy') else 90
        subprocess.run([str(self.binary[variant]), name], check=True, timeout=timeout,
                       env={**{k:v for k,v in os.environ.items() if k!='DAIKIN_NVS_NO_PAIRS'}, **({'DAIKIN_NVS_NO_PAIRS':'1'} if no_pairs else {}), 'ASAN_OPTIONS': 'detect_leaks=' + leaks,'DAIKIN_NVS_PAGES':str(pages)})

    def test_authority_key_type_and_precedence(self):
        self.scenario("authority",pages=4)

    def test_outbound_route_and_key_atomic_at_every_nor_byte(self):
        for variant in ('hub', 'full'):
            for pages in (4,5): self.scenario('route-power',variant,pages=pages)

    def test_live_16k_reclaim_pair_and_six_existing_routes(self):
        self.scenario('live-reclaim',pages=4)

    def test_live_16k_reclaim_and_gc_power_cuts(self):
        self.scenario('live-reclaim-power',pages=4)

    def test_live_16k_old_combined_versions_migrate_atomically(self):
        self.scenario('live-legacy',pages=4)

    def test_live_equivalent_first_authority_migration_power_cuts(self):
        self.scenario("migration","v2",pages=4)
        self.scenario("migration",pages=4)

    def test_dual_legacy_keys_observe_safe_save_budget(self):
        for pages in (4,5): self.scenario("dual-key",pages=pages)

    def test_valid_twelve_devices_and_mapping_budget(self):
        for pages in (4,5):
            if 'rc3' in self.binary:self.scenario('valid-budget','rc3',pages=pages)
            self.scenario('valid-budget',pages=pages,no_pairs=True)
            self.scenario('valid-budget',pages=pages)

    def test_nonbusiness_padding_capacity_or_safe_unchanged_rejection(self):
        for pages in (4,5):
            self.scenario('budget',pages=pages);self.scenario('budget','full',pages=pages)

    def test_password_and_keys_atomic_at_every_nor_byte(self):
        for pages in (4,5):
            self.scenario('power',pages=pages);self.scenario('power','full',pages=pages)

    def test_failed_write_reboot_and_factory_reset(self):
        for pages in (4,5):
            self.scenario('failure',pages=pages);self.scenario('failure','full',pages=pages)

if __name__ == '__main__': unittest.main()
