#include "stock_frame.h"
extern "C" {
#include "protocol/media_wire.h"
}
#include <assert.h>
#include <string>
#include <vector>
#include <stdio.h>
int main(){
 std::vector<unsigned char> payload={0,13,10,'p','u','b',' ',13,10,0,255};
 std::string header="pub SENSOR/001122334455/IPBOX/AABBCCDDEEFF/mqtt/event/upload_data "+std::to_string(payload.size())+" ";
 std::string wire=header+std::string((char*)payload.data(),payload.size())+" 0\r\nversion?\r\n";
 for(size_t chunk=1;chunk<=wire.size();++chunk){
  bry::StockFrame frame;unsigned calls=0;
  auto done=[&](const uint8_t *p,size_t n,size_t at,size_t count){
   if(calls==0){assert(at==header.size());assert(count==payload.size());assert(!memcmp(p+at,payload.data(),count));assert(n==header.size()+count+2);}
   else{assert(calls==1);assert(n==8);assert(!memcmp(p,"version?",8));assert(!at&&!count);}++calls;
  };
  for(size_t i=0;i<wire.size();i+=chunk)for(size_t j=i;j<wire.size()&&j<i+chunk;j++)frame.feed(wire[j],done);
  assert(calls==2);
 }
 bry::StockFrame bad;unsigned calls=0;auto count=[&](const uint8_t*,size_t,size_t,size_t){++calls;};
 std::string oversized="pub test 99999999999999999999 x\r\nversion?\r\n";
 for(auto c:oversized){bad.feed(c,count);}
 assert(calls==1);
 bad.reset();calls=0;std::string longline(5000,'x');longline+="\r\nmac?\r\n";for(auto c:longline)bad.feed(c,count);assert(calls==1);
 // A complete GMv1 packet inside a stock payload must stay with that payload.
 uint8_t encoded[128], command[]={MEDIA_PING,1,0,0,0};
 size_t encoded_size=media_encode(encoded,sizeof(encoded),MEDIA_LINK,123,7,command,sizeof(command));
 assert(encoded_size);
 std::string embedded((char*)encoded,encoded_size);
 embedded.append("\r\n\0",3);
 std::string mixed="pub test "+std::to_string(embedded.size())+" "+embedded+" 0\r\n";
 mixed.append((char*)encoded,encoded_size);
 mixed+="screen_recovery 3 0000007b 1 0 0 00000000 00000000 00000000 00000001\r\n";
 for(size_t chunk=1;chunk<=mixed.size();++chunk){
  bry::StockFrame frame;media_parser parser={};unsigned packets=0,lines=0;
  parser.context=&packets;
  parser.packet=[](void *ctx,unsigned channel,uint32_t session,uint32_t sequence,const uint8_t *p,size_t n){
   assert(channel==MEDIA_LINK&&session==123&&sequence==7&&n==5&&p[0]==MEDIA_PING);
   ++*(unsigned*)ctx;
  };
  auto done=[&](const uint8_t *p,size_t n,size_t at,size_t count){
   if(!lines){assert(at&&count==embedded.size());assert(!memcmp(p+at,embedded.data(),count));}
   else{assert(lines==1&&!at&&!count&&n>16&&!memcmp(p,"screen_recovery ",16));}
   ++lines;
  };
  for(size_t i=0;i<mixed.size();i+=chunk)for(size_t j=i;j<mixed.size()&&j<i+chunk;j++){
   uint8_t c=mixed[j];if(frame.binary||!media_feed(&parser,c,100))frame.feed(c,done);
  }
  assert(lines==2&&packets==1&&!parser.crc_errors&&!parser.framing_errors);
 }
 puts("stock UART: mixed media, all split positions, embedded NUL/CRLF and overflow recovery passed");
}
