"""Exercise G3 provisioning and reconnection with a simulated Wi-Fi driver."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from DAIKIN.tests.test_local_control_priority import function

ROOT = Path(__file__).resolve().parents[2]


class G3WifiRecovery(unittest.TestCase):
    def test_saved_station_retries_and_manual_setup_ap(self):
        main = ROOT / 'DAIKIN/src/main.cpp'
        source = main.read_text()
        retry_state = source[source.index('static constexpr unsigned g3StationAssociated'):
                             source.index('static bool g3NeedsProvisioning()')]
        production = retry_state + '\n'.join(
            function(main, name) for name in (
                'static bool due(',
                'static void startAccessPoint(){',
                'static bool g3NeedsProvisioning()',
                'static void startWifi(){',
            )
        )
        source = (ROOT / 'BRY88BBA15G/wifi_provisioning.inc').read_text()
        production += '\n' + source[:source.index('static bool saveG3WifiTrial()')]
        production += source[source.index('static void clearG3WifiTrial()'):]
        production += '\n' + function(main, 'static bool g3WifiWorkBusy() {')
        production += '\n' + function(main, 'static void pollG3StationRetry(')
        production += '\n' + function(main, 'static void pollWifi(')
        runtime = ROOT / 'BRY88BBA15G/runtime.inc'
        command = function(runtime, 'static void screenCommand(')
        token = command[command.index('  size_t end = 0;'):command.index('  // The maintenance receiver')]
        button = function(runtime, 'if (cmd == "ble_cfg")')
        production += '\nstatic void pressNetworkKey(const char *frame) {\n'
        production += '  const char *p = frame; const size_t n = std::strlen(p);\n'
        production += token + button + '\n}\n'
        fixture = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>

#define IOTCLOUD_BRY88_SCREEN
constexpr int WIFI_STA = 1, WIFI_AP = 2, WIFI_AP_STA = 3, WIFI_PS_MIN_MODEM = 1;
constexpr int WL_DISCONNECTED = 6, WL_CONNECTED = 3, WL_CONNECT_FAILED = 4, WIFI_SCAN_RUNNING = -1;
constexpr int ESP_OK = 0, ESP_FAIL = -1;
enum arduino_event_id_t {
    ARDUINO_EVENT_WIFI_STA_CONNECTED, ARDUINO_EVENT_WIFI_STA_DISCONNECTED,
    ARDUINO_EVENT_WIFI_STA_GOT_IP
};
using String = std::string;
struct wifi_ap_record_t { unsigned char ssid[33]; };
constexpr int WIFI_STORAGE_RAM = 1, WIFI_STORAGE_FLASH = 2;
static uint32_t timeMs = 1000, connectAt;
static bool configRecovery, otaActive, g3OnlineBusy, screenUploadBusy, screenOta;
static bool g3AccessPointConfigured;
static bool screenOtaActive() { return screenOta; }
static bool g3WifiWorkBusy();
static uint32_t millis() { return timeMs; }
static unsigned recoveryApStarts, recoveryApStops, dhcpConfigurations, mdnsStarts, mdnsStops, buttonReplies;
static void screenReply(const std::string &cmd, bool ok, unsigned reason) {
    assert(cmd == "ble_cfg" && !ok && reason == 0x70);
    ++buttonReplies;
}
struct Mdns {
    bool begin(const char *) { ++mdnsStarts; return true; }
    void addService(const char *, const char *, int) {}
    void end() { ++mdnsStops; }
};
static Mdns MDNS;
static void advertiseLanBusiness() {}
struct Hostname { const char *c_str() const { return "g3-test"; } };
static Hostname deviceHostname, apName;
struct IPAddress {
    uint32_t address;
    IPAddress(int a, int b, int c, int d) :
        address(uint32_t(a) << 24 | b << 16 | c << 8 | d) {}
    bool operator!=(IPAddress another) const { return address != another.address; }
};
static void requestBusinessStatusRefresh() {}
static size_t testStrlcpy(char *target, const char *source, size_t size) {
    const size_t length = std::strlen(source);
    assert(length < size);
    std::memcpy(target, source, length + 1);
    return length;
}
#define strlcpy testStrlcpy
static bool saveG3WifiTrial();
struct WifiConfig {
    const char *ssid = "saved-network";
    const char *password = "test-password";
    WifiConfig &operator[](const char *key) {
        field = key;
        return *this;
    }
    const char *operator|(const char *) const {
        return !std::strcmp(field, "ssid") ? ssid : password;
    }
    const char *field = nullptr;
};
static WifiConfig config;
struct WifiDriver {
    int state = WL_DISCONNECTED;
    int scan = 0;
    unsigned attempts = 0;
    bool associated = false, joining = false;
    void setHostname(const char *) {}
    int wifiMode = WIFI_STA;
    std::string attemptedSsid, connectedSsid;
    bool mode(int next) {
        if ((wifiMode & WIFI_AP) && !(next & WIFI_AP)) ++recoveryApStops;
        wifiMode = next;
        return true;
    }
    void disconnect(bool, bool) {
        state = WL_DISCONNECTED;
        associated = joining = false;
    }
    IPAddress localIP() const {
        return state == WL_CONNECTED ? IPAddress(10, 0, 0, 21) : IPAddress(0, 0, 0, 0);
    }
    int getMode() const { return wifiMode; }
    bool softAPConfig(IPAddress, IPAddress, IPAddress) {
        wifiMode |= WIFI_AP;
        ++dhcpConfigurations;
        return true;
    }
    bool softAP(const char *) {
        ++recoveryApStarts;
        return true;
    }
    void softAPdisconnect(bool disable) {
        assert(disable);
        wifiMode &= ~WIFI_AP;
        ++recoveryApStops;
    }
    void setSleep(int mode) { assert(mode == WIFI_PS_MIN_MODEM); }
    int status() const { return state; }
    int scanComplete() const { return scan; }
    int begin(const char *ssid, const char *) {
        attemptedSsid = ssid;
        mode(wifiMode | WIFI_STA);
        ++attempts;
        joining = true;
        return state;
    }
};
static WifiDriver WiFi;
static int esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap) {
    if (!WiFi.associated) return ESP_FAIL;
    strlcpy(reinterpret_cast<char *>(ap->ssid), WiFi.connectedSsid.c_str(), sizeof(ap->ssid));
    return ESP_OK;
}
static int driverStorage = WIFI_STORAGE_FLASH;
static int esp_wifi_set_storage(int storage) { driverStorage = storage; return ESP_OK; }
static int esp_wifi_connect() {
    if (WiFi.joining)
        return ESP_FAIL;
    WiFi.joining = true;
    ++WiFi.attempts;
    return ESP_OK;
}
'''
        scenarios = r'''
static unsigned saves = 0;
static std::string savedSsid, savedPassword;
static bool saveSucceeds = true;
static bool saveG3WifiTrial() {
    ++saves;
    if (!saveSucceeds) return false;
    savedSsid = g3TrialSsid;
    savedPassword = g3TrialPassword;
    config.ssid = savedSsid.c_str();
    config.password = savedPassword.c_str();
    return true;
}
static void poll(uint32_t now) { timeMs = now; pollWifi(now); }
static void loseStation() {
    WiFi.disconnect(false, false);
    g3WifiEvent(ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
}
static void associateStation(const char *ssid) {
    WiFi.associated = true;
    WiFi.joining = false;
    WiFi.connectedSsid = ssid;
    g3WifiEvent(ARDUINO_EVENT_WIFI_STA_CONNECTED);
}
static void gotIp() {
    WiFi.state = WL_CONNECTED;
    g3WifiEvent(ARDUINO_EVENT_WIFI_STA_GOT_IP);
}
int main() {
    startWifi();
    assert(WiFi.attempts == 1 && WiFi.getMode() == WIFI_STA);
    uint32_t now = 2000;
    for (unsigned retries = 0; retries < 4; ++retries) {
        loseStation();
        poll(now);
        poll(now + 14999);
        assert(WiFi.attempts == retries + 1 && recoveryApStarts == 0);
        now += 15000;
        poll(now);
        assert(WiFi.attempts == retries + 2);
        now += 2000;
    }
    loseStation();
    poll(now);
    poll(now + 29999);
    assert(WiFi.attempts == 5);
    WiFi.scan = WIFI_SCAN_RUNNING;
    poll(now + 30000);
    assert(WiFi.attempts == 5);
    WiFi.scan = 0;
    bool *busy[] = {&otaActive, &screenOta, &screenUploadBusy, &g3OnlineBusy};
    for (bool *state : busy) {
        *state = true;
        poll(now + 30000);
        assert(WiFi.attempts == 5);
        *state = false;
    }
    now += 30001;
    poll(now);
    assert(WiFi.attempts == 6 && !recoveryApStarts);
    associateStation("saved-network");
    gotIp();
    poll(now + 1000);
    assert(g3FastRetries == 0 && mdnsStarts == 1);

    // The key disconnects STA even if the old router is still in range.
    timeMs = now + 2000;
    pressNetworkKey("ble_cfg 1800");
    assert(recoveryApStarts == 1 && WiFi.getMode() == WIFI_AP);
    assert(!WiFi.associated && WiFi.attempts == 6 && !std::strcmp(config.ssid, "saved-network"));
    const uint32_t deadline = g3WifiDeadline;
    for (unsigned n = 1; n <= 3; ++n) {
        poll(now + 2000 + n * 2200);
        pressNetworkKey("ble_cfg 1800");
    }
    assert(dhcpConfigurations == 1 && buttonReplies == 4 && g3WifiDeadline == deadline);
    poll(deadline - 1);
    assert(WiFi.attempts == 6 && WiFi.getMode() == WIFI_AP);
    poll(deadline);
    assert(WiFi.getMode() == WIFI_STA && WiFi.attemptedSsid == "saved-network" && WiFi.attempts == 7);

    // Commit and close the setup AP only after DHCP on the new network.
    associateStation("saved-network");
    gotIp();
    poll(deadline + 1000);
    timeMs = deadline + 5000;
    pressNetworkKey("ble_cfg");
    assert(queueG3WifiTrial("new-network", "new-password"));
    assert(!queueG3WifiTrial("other", ""));
    poll(timeMs + 10);
    assert(driverStorage == WIFI_STORAGE_RAM && WiFi.attemptedSsid == "new-network" &&
           !std::strcmp(config.ssid, "saved-network"));
    associateStation("new-network");
    poll(timeMs + 10);
    assert(saves == 0 && WiFi.getMode() == WIFI_AP_STA);
    gotIp();
    poll(timeMs + 10);
    assert(saves == 1 && !std::strcmp(config.ssid, "new-network") &&
           !std::strcmp(g3WifiTrialStatus, "success") && driverStorage == WIFI_STORAGE_FLASH);
    const uint32_t connectedAt = timeMs;
    pressNetworkKey("ble_cfg 1800");
    assert(WiFi.getMode() == WIFI_STA && recoveryApStarts == 2 && g3TrialPassword[0] == 0);

    // An unrelated DHCP result or storage failure restores the prior connection.
    timeMs = connectedAt + 6000;
    pressNetworkKey("ble_cfg");
    assert(queueG3WifiTrial("bad-network", "bad-password"));
    poll(timeMs + 100);
    associateStation("unrelated");
    gotIp();
    poll(timeMs + 300);
    assert(saves == 1);
    poll(g3WifiDeadline);
    assert(!std::strcmp(g3WifiTrialStatus, "timeout_rollback") && (WiFi.getMode() & WIFI_AP));
    poll(g3WifiDeadline);
    assert(!std::strcmp(g3WifiTrialStatus, "timeout_rollback") &&
           WiFi.attemptedSsid == "new-network" && WiFi.getMode() == WIFI_STA);
    timeMs += 6000;
    pressNetworkKey("ble_cfg");
    assert(queueG3WifiTrial("another-network", ""));
    poll(timeMs + 100);
    associateStation("another-network");
    gotIp();
    saveSucceeds = false;
    poll(timeMs + 100);
    assert(!std::strcmp(g3WifiTrialStatus, "storage_failure") && (WiFi.getMode() & WIFI_AP));
    poll(g3WifiDeadline);
    assert(!std::strcmp(g3WifiTrialStatus, "storage_failure") &&
           WiFi.attemptedSsid == "new-network" && driverStorage == WIFI_STORAGE_FLASH);
    saveSucceeds = true;

    // A first-time setup remains available if there is no network to restore.
    loseStation();
    config.ssid = "";
    startWifi();
    assert(queueG3WifiTrial("first-network", ""));
    poll(timeMs + 10);
    poll(g3WifiDeadline);
    assert((WiFi.getMode() & WIFI_AP) && !*config.ssid);

    WiFi.mode(WIFI_STA);
    config.ssid = "new-network";
    startWifi();
    loseStation();
    poll(UINT32_MAX - 100);
    const unsigned attempts = WiFi.attempts;
    poll(14898);
    assert(WiFi.attempts == attempts);
    poll(14899);
    assert(WiFi.attempts == attempts + 1);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'wifi_recovery.cpp'
            binary = source.with_suffix('')
            source.write_text(fixture + production + '\n' + scenarios)
            subprocess.run(
                ['c++', '-std=c++11', '-Wall', '-Wextra', '-Werror',
                 '-fsanitize=address,undefined', str(source), '-o', str(binary)],
                check=True,
            )
            subprocess.run([str(binary)], check=True, timeout=10)
