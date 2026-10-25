#include "climate.h"
#include <assert.h>
#include <stdio.h>

int main() {
  // Independently evaluated stock-firmware equations (Celsius / percent RH).
  const float cases[][4]={{30,40,25.1f,48.88305f},{25,40,20.1f,49.86937f},
                         {33.7f,40,28.8f,48.2684f}};
  for(const auto &v:cases){
    float t,h;assert(daikin::compensateClimate(v[0],v[1],t,h));
    assert(fabsf(t-v[2])<0.0001f && fabsf(h-v[3])<0.002f);
  }
  for(int t=-40;t<=85;t++)for(int h=0;h<=100;h++){
    float ct,ch;assert(daikin::compensateClimate(t,h,ct,ch));
    assert(isfinite(ct)&&isfinite(ch)&&ch>=0&&ch<=100);
  }
  float t,h;
  assert(daikin::compensateClimate(30,40,t,h,0,5));
  assert(t==30 && h>40 && h<42);
  assert(!daikin::compensateClimate(30,40,t,h,21,0));
  assert(!daikin::compensateClimate(30,40,t,h,4.9f,NAN));
  assert(!daikin::compensateClimate(NAN,40,t,h));
  assert(!daikin::compensateClimate(25,INFINITY,t,h));
  assert(!daikin::compensateClimate(25,-1,t,h));
  assert(!daikin::compensateClimate(126,40,t,h));
  puts("stock climate vectors, operating range and invalid inputs PASS");
}
