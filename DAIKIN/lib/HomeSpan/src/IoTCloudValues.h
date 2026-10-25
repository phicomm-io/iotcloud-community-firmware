/* SPDX-License-Identifier: MIT */
#pragma once
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace iotcloud {
inline bool homeValueSpace(char c){
  return c==' ' || c=='\t' || c=='\n' || c=='\r';
}
inline bool homeValueBounds(const char *text,const char *&begin,const char *&end){
  if(!text)return false;
  begin=text;end=text+std::strlen(text);
  while(begin<end && homeValueSpace(*begin))++begin;
  while(end>begin && homeValueSpace(end[-1]))--end;
  return begin<end;
}
inline bool homeUnsignedDigits(const char *begin,const char *end,uint64_t maximum,uint64_t &result){
  if(begin==end || (end-begin>1 && *begin=='0'))return false;
  uint64_t value=0;
  for(auto p=begin;p<end;++p){
    if(*p<'0' || *p>'9')return false;
    const unsigned digit=*p-'0';
    if(value>maximum/10 || (value==maximum/10 && digit>maximum%10))return false;
    value=value*10+digit;
  }
  result=value;return true;
}
// Parse a complete JSON scalar into its native width before narrowing or
// changing characteristic state. All outputs are untouched on failure.
inline bool homeParseUnsigned(const char *text,uint64_t minimum,uint64_t maximum,uint64_t &result){
  const char *begin,*end;uint64_t value;
  if(minimum>maximum || !homeValueBounds(text,begin,end) ||
     !homeUnsignedDigits(begin,end,maximum,value) || value<minimum)return false;
  result=value;return true;
}
inline bool homeParseInt(const char *text,int32_t minimum,int32_t maximum,int32_t &result){
  const char *begin,*end;uint64_t magnitude;
  if(minimum>maximum || !homeValueBounds(text,begin,end))return false;
  const bool negative=*begin=='-';
  if(negative)++begin;
  if(!homeUnsignedDigits(begin,end,uint64_t(INT32_MAX)+(negative?1:0),magnitude))return false;
  const int64_t value=negative?-int64_t(magnitude):int64_t(magnitude);
  if(value<minimum || value>maximum)return false;
  result=int32_t(value);return true;
}
inline bool homeParseBool(const char *text,bool &result){
  const char *begin,*end;
  if(!homeValueBounds(text,begin,end))return false;
  const size_t length=end-begin;
  if((length==1 && *begin=='0') || (length==5 && !std::memcmp(begin,"false",5))){result=false;return true;}
  if((length==1 && *begin=='1') || (length==4 && !std::memcmp(begin,"true",4))){result=true;return true;}
  return false;
}
inline bool homeParseFloat(const char *text,double minimum,double maximum,double &result){
  const char *begin,*end;
  if(!std::isfinite(minimum) || !std::isfinite(maximum) || minimum>maximum ||
     !homeValueBounds(text,begin,end))return false;
  auto p=begin;
  if(*p=='-')++p;
  if(p==end)return false;
  if(*p=='0')++p;
  else {
    if(*p<'1' || *p>'9')return false;
    do {++p;}while(p<end && *p>='0' && *p<='9');
  }
  if(p<end && *p=='.'){
    ++p;const auto digits=p;
    while(p<end && *p>='0' && *p<='9')++p;
    if(p==digits)return false;
  }
  if(p<end && (*p=='e' || *p=='E')){
    ++p;if(p<end && (*p=='+' || *p=='-'))++p;
    const auto digits=p;
    while(p<end && *p>='0' && *p<='9')++p;
    if(p==digits)return false;
  }
  if(p!=end)return false;
  errno=0;char *parsedEnd;
  const double value=std::strtod(begin,&parsedEnd);
  if(parsedEnd!=end || errno==ERANGE || !std::isfinite(value) || value<minimum || value>maximum)return false;
  result=value;return true;
}
// setValidValues() produces this trusted, compact integer array locally.
inline bool homeValueInList(const char *list,int64_t value){
  if(!list)return true;
  if(*list++!='[')return false;
  bool found=false;
  while(*list && *list!=']'){
    const char *begin=list;
    while(*list && *list!=',' && *list!=']')++list;
    const bool negative=*begin=='-';
    if(negative)++begin;
    uint64_t magnitude;
    if(!homeUnsignedDigits(begin,list,UINT32_MAX,magnitude))return false;
    if((negative?-int64_t(magnitude):int64_t(magnitude))==value)found=true;
    if(*list==','){++list;if(!*list || *list==']')return false;}
  }
  return *list==']' && !list[1] && found;
}
} // namespace iotcloud
