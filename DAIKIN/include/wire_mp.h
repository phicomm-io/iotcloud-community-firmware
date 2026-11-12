/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <ArduinoJson.h>
#include <algorithm>
#include <cstring>
// ArduinoJson 6.21.5's Arduino String writer flushes with concat(char*),
// truncating MessagePack at NUL bytes. Always append with an explicit length.
struct WireStringWriter {
  String &out;
  size_t write(uint8_t byte){const char text[]={char(byte),0};return out.concat(text,1)?1:0;}
  size_t write(const uint8_t *bytes,size_t length){
    size_t written=0;while(written<length){char scratch[65];size_t n=std::min(size_t(64),length-written);memcpy(scratch,bytes+written,n);scratch[n]=0;if(!out.concat(scratch,n))break;written+=n;}return written;
  }
};
template<class T> static size_t serializeWireMP(const T &value,String &out){
  const size_t expected=measureMsgPack(value);out="";if(!out.reserve(expected))return 0;
  WireStringWriter writer{out};const size_t written=serializeMsgPack(value,writer);
  if(written!=expected||out.length()!=expected){out="";return 0;}return written;
}
