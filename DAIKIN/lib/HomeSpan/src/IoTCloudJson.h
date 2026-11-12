/* SPDX-License-Identifier: MIT */
#pragma once
#include "IoTCloudValues.h"
#include "../../../../common/wire_format.h"

namespace iotcloud {
inline void homeJsonSpace(char *&p){while(homeValueSpace(*p))++p;}
inline bool homeJsonTake(char *&p,char c){homeJsonSpace(p);if(*p!=c)return false;++p;return true;}
inline int homeJsonHex(char c){return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;}
inline bool homeJsonQuoted(char *&p){
  if(*p++!='"')return false;
  while(*p && *p!='"'){
    if(uint8_t(*p)<32)return false;
    if(*p++!='\\')continue;
    if(*p=='u'){
      ++p;for(unsigned i=0;i<4;++i){if(homeJsonHex(*p)<0)return false;++p;}
    }else{if(!*p || !std::strchr("\"\\/bfnrt",*p))return false;++p;}
  }
  if(!*p)return false;
  ++p;return true;
}
// HAP characteristic writes contain at most 32 flat objects with scalar values.
// Scan once without mutation before sizing/dispatch, then terminate borrowed
// scalar tokens in place. Delimiters inside JSON strings are never separators.
template<class Field> int homeJsonRows(char *p,bool terminate,Field field){
  if(!p || !homeJsonTake(p,'{'))return 0;
  homeJsonSpace(p);
  const char key[]="\"characteristics\"";
  if(std::strncmp(p,key,sizeof(key)-1))return 0;
  p+=sizeof(key)-1;
  if(!homeJsonTake(p,':') || !homeJsonTake(p,'['))return 0;
  int rows=0;
  do{
    if(rows>=32 || !homeJsonTake(p,'{'))return 0;
    unsigned found=0;bool more;
    do{
      homeJsonSpace(p);char *keyStart=p;
      if(!homeJsonQuoted(p))return 0;
      const size_t length=p-keyStart;
      const char *keys[]={"\"aid\"","\"iid\"","\"value\"","\"ev\"","\"r\"","\"pid\""};
      unsigned k=0;
      while(k<6 && (length!=std::strlen(keys[k]) || std::memcmp(keyStart,keys[k],length)))++k;
      if(k==6 || (found&(1u<<k)) || !homeJsonTake(p,':'))return 0;
      found|=1u<<k;homeJsonSpace(p);char *value=p;
      if(*p=='"'){if(!homeJsonQuoted(p))return 0;}
      else{
        while(*p && !homeValueSpace(*p) && *p!=',' && *p!='}' && *p!=']'){
          if(*p=='{' || *p=='[' || *p=='"')return 0;
          ++p;
        }
        if(p==value)return 0;
      }
      char *end=p;homeJsonSpace(p);
      if(*p!=',' && *p!='}')return 0;
      more=*p++==',';
      if(terminate)*end=0;
      if(!field(rows,k,value))return 0;
    }while(more);
    if((found&15)!=7 && (found&15)!=11 && (found&15)!=15)return 0;
    ++rows;homeJsonSpace(p);
    if(*p!=',')break;
    ++p;
  }while(true);
  if(!homeJsonTake(p,']') || !homeJsonTake(p,'}'))return 0;
  homeJsonSpace(p);return *p?0:rows;
}
inline bool homeJsonCode(char *&p,uint32_t &code){
  code=0;
  for(unsigned i=0;i<4;++i){const int digit=homeJsonHex(*p);if(digit<0)return false;code=code*16+digit;++p;}
  return true;
}
// Decode into the borrowed request buffer. UTF-8 never expands beyond its JSON
// spelling; reject NUL, incomplete escapes and unpaired UTF-16 surrogates.
inline char *homeJsonString(char *text){
  const char *begin,*end;
  if(!homeValueBounds(text,begin,end) || end-begin<2 || *begin!='"' || end[-1]!='"')return nullptr;
  char *read=const_cast<char *>(begin+1),*out=read,*start=read;
  while(read<end-1){
    unsigned char c=*read++;
    if(c=='"' || c<32)return nullptr;
    if(c!='\\'){*out++=char(c);continue;}
    if(read>=end-1)return nullptr;
    c=*read++;
    if(c=='u'){
      if(end-1-read<4)return nullptr;
      uint32_t code;if(!homeJsonCode(read,code))return nullptr;
      if(code>=0xd800 && code<=0xdbff){
        if(end-1-read<6 || read[0]!='\\' || read[1]!='u')return nullptr;
        read+=2;uint32_t low;
        if(!homeJsonCode(read,low) || low<0xdc00 || low>0xdfff)return nullptr;
        code=0x10000+(code-0xd800)*1024+low-0xdc00;
      }else if(code>=0xdc00 && code<=0xdfff)return nullptr;
      if(!code)return nullptr;
      if(code<0x80)*out++=char(code);
      else{
        if(code>=0x10000){*out++=char(0xf0|(code>>18));*out++=char(0x80|((code>>12)&63));}
        else if(code>=0x800)*out++=char(0xe0|(code>>12));
        if(code>=0x800)*out++=char(0x80|((code>>6)&63));
        else *out++=char(0xc0|(code>>6));
        *out++=char(0x80|(code&63));
      }
    }else{
      const char *escapes="\"\\/bfnrt",*match=std::strchr(escapes,c);
      if(!match)return nullptr;
      *out++="\"\\/\b\f\n\r\t"[match-escapes];
    }
  }
  *out=0;
  return ph_wire_utf8(reinterpret_cast<const unsigned char *>(start),out-start)?start:nullptr;
}
template<class Text> Text homeJsonQuote(const char *text){
  Text out="\"";
  for(const unsigned char *p=reinterpret_cast<const unsigned char *>(text);p && *p;++p){
    if(*p=='"' || *p=='\\'){out+='\\';out+=char(*p);}
    else if(*p<32){out+="\\u00";out+="0123456789abcdef"[*p>>4];out+="0123456789abcdef"[*p&15];}
    else out+=char(*p);
  }
  out+='"';return out;
}
}
