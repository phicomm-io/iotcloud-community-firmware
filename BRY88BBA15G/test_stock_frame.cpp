#include "stock_frame.h"
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
 puts("stock UART: all split positions, embedded NUL/CRLF, following frames and overflow recovery passed");
}
