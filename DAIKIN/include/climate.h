#pragma once
#include <math.h>

namespace daikin {
inline bool validClimateOffsets(float temperature,float humidity) {
  return isfinite(temperature)&&isfinite(humidity)&&temperature>=-20&&temperature<=20&&humidity>=-30&&humidity<=30;
}
// Recovered from this DUT's original firmware: 0x400e551c / 0x400e564c /
// 0x400e5794. Celsius and percent RH in/out; pressure in Pa, mixing ratio g/kg.
inline double saturationPressure(float celsius) {
  const float k=celsius+273.15f;
  float first=10.795740127563477f*(1.0f-273.1600036621094f/k)+0.7861400246620178f;
  const double ratio=double(k)/273.16;
  const double exponent=first-5.0279998779296875*log10(ratio)
    +0.00011504750000312924*(1.0-pow(10.0,-8.296899795532227*(ratio-1.0)))
    +0.0004287300107534975*(pow(10.0,4.76954984664917*(1.0-1.0/ratio))-1.0);
  return 100.0*pow(10.0,exponent);
}
inline bool compensateClimate(float rawT,float rawH,float &t,float &h,float temperatureOffset=4.9f,float humidityOffset=0.0f) {
  if(!validClimateOffsets(temperatureOffset,humidityOffset))return false;
  if(!isfinite(rawT)||!isfinite(rawH)||rawT< -40||rawT>125||rawH<0||rawH>100)return false;
  t=rawT-temperatureOffset;
  const float vapor=saturationPressure(rawT);
  const float denominator=10132500.0f-rawH*vapor;
  if(denominator<=0)return false;
  const float mixing=622.0f*rawH*vapor/denominator;
  const float corrected=mixing*0.8915f+0.2632f;
  const float coolVapor=saturationPressure(t);
  h=10132500.0f*corrected/(coolVapor*(622.0f+corrected));
  if(!isfinite(h))return false;
  // Keep relative humidity within its physical range at extreme inputs.
  h=fminf(100.0f,fmaxf(0.0f,h+humidityOffset));
  return true;
}
}
