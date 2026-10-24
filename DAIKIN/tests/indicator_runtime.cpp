// Run the production GPIO/status adapter with a fake clock, Wi-Fi and GPIOs.
#include "indicator_json.h"
#include <assert.h>
#include <stdio.h>
enum Sensor { PM25, CO2, TEMPERATURE, HUMIDITY, TVOC, HCHO, PM10, SENSOR_COUNT };
static daikin::Sample readings[SENSOR_COUNT];
static DynamicJsonDocument config(6144);
static daikin::LightSettings lightSettings=daikin::defaultLightSettings();
static bool indicator=true,customLightSettings=false,hchoPresent=false,mqttConnected=false,mqttCommandReady=false;
static constexpr int WL_CONNECTED=3;
static struct {int connected=0;int status()const{return connected;}} WiFi;
static bool pins[40];
static void digitalWrite(unsigned pin,bool on){pins[pin]=on;}
#include "../src/indicators.inc"

int main(){
  using namespace daikin;
  readings[CO2].set(1500,0);readings[TVOC].set(400,0);readings[PM25].set(100,0);readings[PM10].set(10,0);
  pollIndicators(0);assert(pins[4]&&pins[32]&&pins[33]&&pins[25]);
  // The business-light switch never suppresses provisioning/network signals.
  indicator=false;pollIndicators(800);assert(!pins[4]&&!pins[32]&&!pins[33]&&!pins[25]);
  config["wifi"]["ssid"]="test";
  pollIndicators(1000);assert(pins[25]);pollIndicators(1149);assert(pins[25]);pollIndicators(1150);assert(!pins[25]);
  WiFi.connected=WL_CONNECTED;config["mqtt"]["host"]="test.invalid";
  pollIndicators(2000);assert(pins[25]);pollIndicators(2999);assert(pins[25]);pollIndicators(3000);assert(!pins[25]);
  mqttConnected=true;pollIndicators(3100);assert(!pins[25]); // Connected without command SUBACK remains slow.
  mqttCommandReady=true;pollIndicators(3200);assert(pins[25]);
  config["mqtt"]["host"]="";mqttConnected=false;mqttCommandReady=false;pollIndicators(3300);assert(pins[25]);
  // Select PM10: a high unselected PM2.5 reading must have no effect.
  indicator=true;customLightSettings=true;lightSettings.pm10=true;
  lightSettings.metrics[LightPM].rules[0].threshold=120;
  lightSettings.metrics[LightPM].rules[0].mode=LightMode::Fast;
  readings[PM25].set(9999,4000);readings[PM10].set(119,4000);
  pollIndicators(4000);assert(!pins[33]);
  readings[PM10].set(121,4100);pollIndicators(4100);assert(pins[33]);
  pollIndicators(4250);assert(!pins[33]);
  lightSettings.pm10=false;pollIndicators(4300);assert(!pins[33]); // Same pattern retains its phase.
  readings[PM25].set(119,4400);pollIndicators(4400);assert(!pins[33]);
  readings[PM10].set(9999,4400);pollIndicators(4450);assert(!pins[33]);
  readings[PM25].set(121,4500);pollIndicators(4500);assert(pins[33]);
  pollIndicators(24500);assert(!pins[33]&&!pins[4]&&!pins[32]); // Expiry, even when the last reading was high.
  // Original automatic VOC/HCHO choice is preserved until custom rules are saved.
  readings[TVOC].set(400,25000);readings[HCHO].set(0.01f,25000);hchoPresent=true;customLightSettings=false;
  pollIndicators(25000);assert(!pins[32]);
  customLightSettings=true;pollIndicators(25001);assert(pins[32]);
  DynamicJsonDocument state(6144);indicatorStatus(state.to<JsonObject>(),25001);
  assert(!state.overflowed());assert(state["indicator_rules"]["pm_source"]=="pm25");
  assert(state["indicator_modes"]["tvoc"]=="on"&&state["system_indicator"]=="ready");
  puts("PASS production GPIO adapter: selected PM source, freshness, VOC migration and Wi-Fi/MQTT readiness signals");
}
