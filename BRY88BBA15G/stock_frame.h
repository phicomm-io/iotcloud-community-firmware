/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
namespace bry {
// pub payload is length-delimited and may contain CR, LF and NUL. Never pass
// it through the text/capability parser. Overflow discards to a text boundary.
struct StockFrame {
 static constexpr size_t limit=4096;
 uint8_t bytes[limit];size_t used=0,payloadAt=0,payloadSize=0;bool binary=false,discard=false;
 void reset(){used=payloadAt=payloadSize=0;binary=discard=false;}
 template<class F> void feed(uint8_t c,F complete){
  if(discard){if(c=='\n')reset();return;}
  if(used==limit){reset();discard=true;return;}
  bytes[used++]=c;
  if(!binary&&used>=4&&!memcmp(bytes,"pub ",4)){
   size_t topic=4;while(topic<used&&bytes[topic]!=' ')++topic;
   if(topic<used){size_t pos=topic+1,n=0;bool digits=false;
    while(pos<used&&bytes[pos]>='0'&&bytes[pos]<='9'){digits=true;n=n*10+bytes[pos++]-'0';if(n>2048){reset();discard=true;return;}}
    if(pos<used){if(!digits||bytes[pos]!=' '){reset();discard=true;return;}binary=true;payloadAt=pos+1;payloadSize=n;}
   }
  }
  if(binary&&used<payloadAt+payloadSize)return;
  if(c=='\n'&&used>=2&&bytes[used-2]=='\r'){
   if(!binary||used>=payloadAt+payloadSize+2){complete(bytes,used-2,payloadAt,payloadSize);reset();}
  }
 }
};
}
