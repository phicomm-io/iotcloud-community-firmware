/* Shared M1/A1 adapter, compiled once per firmware. */
#include "service.h"
#include "firmware_variant.h"
#include "feature_mp.h"
#ifdef FEATURE_DEVICE_A1
#include "a1.h"
#include "filter.h"
#ifdef A1_FRESH_AIR
#include "fresh_air.h"
#endif
#define DEVICE_MODEL "a1"
#define CONFIG a1_config_t
#define STATE a1_state_t
#define GET_CONFIG a1_config_get
#define GET_STATE a1_state_get
#define SAVE_CONFIG a1_config_save
#define SAVE_DEVICE_CONFIG a1_device_config_save
#define REBOOT a1_reboot_later
#define RESET a1_reset
#define WIFI_TRIAL a1_wifi_trial
#define SCAN a1_scan_start
#define DOWNLOAD a1_ota_download
#define DEVICE_NAME_PREFIX "A1"
#define DEVICE_ID a1_id
#define DEVICE_VERSION A1_VERSION
#else
#include "m1.h"
#define DEVICE_MODEL "m1"
#define CONFIG m1_config_t
#define STATE m1_state_t
#define GET_CONFIG m1_config_get
#define GET_STATE m1_state_get
#define SAVE_CONFIG m1_config_save
#define SAVE_DEVICE_CONFIG m1_device_config_save
#define REBOOT m1_reboot_later
#define RESET m1_reset
#define WIFI_TRIAL m1_wifi_trial
#define SCAN m1_scan_start
#define DOWNLOAD m1_ota_download
#define DEVICE_NAME_PREFIX "M1"
#define DEVICE_ID m1_id
#define DEVICE_VERSION M1_VERSION
#endif
#include "device_status.h"
static const char *const svc_capability_actions[]={"schedules.get","schedules.put","schedules.delete","schedules.enable","buttons.get","status.get","capabilities.get","config.get","logs.get","config.set","wifi.scan","wifi.scan.get","binding.status.get","system.reboot","system.reset","ota.start","night.get","night.set","time.get","time.set","binding.get",
#ifdef FEATURE_DEVICE_A1
    "binding.set",
#endif

#ifdef FEATURE_DEVICE_A1
#ifdef A1_FRESH_AIR
    "fresh_air.set","fresh_air.config.get","fresh_air.config.set","fresh_air.linkage.get","fresh_air.linkage.set",
#endif
    "filter.model.set","filter.get","filter.set","filter.reset","light_effect.next","light_effect.select","light_always.set","sensors.average.get","buttons.set","binding.probe","light_effect.get","light_effect.set","fan.set","fan.mode.set","fan.mode.toggle","fan.panel","light.set","child_lock.set","linkage.get","linkage.set",
#else
    "sensor_details.get","display.set","buzzer.set","mcu.version.get","mcu.version.query","alarm.get","alarm.set","co2_display.get","co2_display.set","co2_source.get","co2_source.set",
    "display_source.get","display_source.set","display_data.set",
    "display_profile.get","display_profile.set","display_value.set",
    "wake_alarm.get","wake_alarm.start","wake_alarm.stop",
    "music.get","music.begin","music.chunk","music.commit","music.read","music.play","music.stop","music.cancel",
#endif
    NULL};
#ifdef FEATURE_DEVICE_A1
static const char *const svc_capability_keys[]={"power","wifi","combination",NULL};
#endif

#include "service_product_mp.inc"
