/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <Arduino.h>
#include <atomic>
#include <utility>
#include <freertos/semphr.h>

// One application writer, HTTP task readers. No I/O or JSON work under the
// mutex. The application writer never waits; HTTP may wait at most one RTOS
// tick for a short publication swap, then reports busy on sustained contention.
// The lease prevents a stalled application from serving indefinitely valid
// sensor flags. Strings are never borrowed outside a locked copy.
class HttpReadSnapshot {
  SemaphoreHandle_t mutex_=nullptr;
  std::atomic<bool> valid_{false};
  std::atomic<uint32_t> checkedAt_{0};
  uint32_t validUntil_=0;
  bool hasDeadline_=false;
  std::atomic<uint32_t> unavailable_{0},expired_{0},contended_{0},allocationFailed_{0};
  String data_,envelope_;
public:
  // Called once during setup, before the HTTP task starts.
  bool begin(){mutex_=xSemaphoreCreateMutex();return mutex_!=nullptr;}
  void invalidate(){valid_.store(false,std::memory_order_release);}
  void refresh(uint32_t now){checkedAt_.store(now,std::memory_order_release);}
  bool publish(String &&data,String &&envelope,uint32_t now,uint32_t freshFor=0){
    // A normal refresh prepares its replacement before acquiring this lock.
    // Contention must leave the previous complete publication readable.
    if(!mutex_||xSemaphoreTake(mutex_,0)!=pdTRUE)return false;
    data_=std::move(data);envelope_=std::move(envelope);hasDeadline_=freshFor!=0;validUntil_=now+freshFor;refresh(now);
    valid_.store(true,std::memory_order_release);xSemaphoreGive(mutex_);return true;
  }
  bool copy(String &out,bool envelope,uint32_t now){
    // The caller captures now before entry. The writer may refresh after that
    // capture, so a negative signed age means a newer publication, not expiry.
    // Use the same wrap-safe half-range clock ordering as the owner deadlines.
    if(!valid_.load(std::memory_order_acquire)){++unavailable_;return false;}
    if(int32_t(now-checkedAt_.load(std::memory_order_acquire))>2000){++expired_;return false;}
    if(!mutex_||xSemaphoreTake(mutex_,1)!=pdTRUE){++contended_;return false;}
    // Refreshing the owner lease cannot extend any sample's validity.
    if(hasDeadline_&&int32_t(now-validUntil_)>=0){++expired_;xSemaphoreGive(mutex_);return false;}
    const String &source=envelope?envelope_:data_;
    bool ok=valid_.load(std::memory_order_acquire)&&source.length()&&out.reserve(source.length());
    if(ok){out="";ok=out.concat(source.c_str(),source.length());}
    ok=ok&&valid_.load(std::memory_order_acquire);
    if(!ok){if(!valid_.load(std::memory_order_acquire))++unavailable_;else ++allocationFailed_;}
    xSemaphoreGive(mutex_);return ok;
  }
  uint32_t unavailable()const{return unavailable_.load();}
  uint32_t expired()const{return expired_.load();}
  uint32_t contended()const{return contended_.load();}
  uint32_t allocationFailed()const{return allocationFailed_.load();}
  // Application-owner access only. HTTP always uses copy().
  const String &data()const{return data_;}
  const String &envelope()const{return envelope_;}
};
