/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "a1_decimal.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* 3072 bits cover the exact decimal expansion of any binary64 (including
 * 5^1074), and every <=349-byte decimal capable of rounding to a finite value.
 * No heap, locale, errno, floating arithmetic, or generic printf dependency. */
#define PD_WORDS 96u
typedef struct {uint32_t word[PD_WORDS];unsigned used;} pd_big;
static void pd_set(pd_big *a,uint64_t n){memset(a,0,sizeof(*a));a->word[0]=(uint32_t)n;a->word[1]=(uint32_t)(n>>32);a->used=a->word[1]?2:n?1:0;}
static void pd_trim(pd_big *a){while(a->used&&!a->word[a->used-1])a->used--;}
static bool pd_mul(pd_big *a,unsigned n,unsigned add){uint64_t carry=add;for(unsigned i=0;i<a->used;i++){carry+=(uint64_t)a->word[i]*n;a->word[i]=(uint32_t)carry;carry>>=32;}if(carry){if(a->used==PD_WORDS)return false;a->word[a->used++]=(uint32_t)carry;}return true;}
static unsigned pd_bits(const pd_big *a){return a->used?(a->used-1)*32+32-(unsigned)__builtin_clz(a->word[a->used-1]):0;}
static uint32_t pd_word(const pd_big *a,unsigned i,unsigned shift){unsigned whole=shift/32,part=shift%32;if(i<whole)return 0;i-=whole;uint32_t n=i<a->used?a->word[i]<<part:0;if(part&&i&&i-1<a->used)n|=a->word[i-1]>>(32-part);return n;}
/* Compare a with b*2^shift, without a temporary large integer. */
static int pd_cmp(const pd_big *a,const pd_big *b,unsigned shift){unsigned ab=pd_bits(a),bb=pd_bits(b);if(bb)bb+=shift;if(ab!=bb)return ab>bb?1:-1;unsigned used=(ab+31)/32;while(used--){uint32_t av=a->word[used],bv=pd_word(b,used,shift);if(av!=bv)return av>bv?1:-1;}return 0;}
static bool pd_shift(pd_big *a,unsigned shift){unsigned bits=pd_bits(a);if(!bits)return true;if(shift>PD_WORDS*32-bits)return false;unsigned used=(bits+shift+31)/32;for(unsigned i=used;i--;)a->word[i]=pd_word(a,i,shift);a->used=used;return true;}
static void pd_sub(pd_big *a,const pd_big *b,unsigned shift){uint64_t borrow=0;for(unsigned i=0;i<a->used;i++){uint64_t n=(uint64_t)pd_word(b,i,shift)+borrow;uint32_t before=a->word[i];a->word[i]=before-(uint32_t)n;borrow=(n>>32)+(before<(uint32_t)n);}pd_trim(a);}
static unsigned pd_div10(pd_big *a){uint64_t remainder=0;for(unsigned i=a->used;i--;){uint64_t n=(remainder<<32)|a->word[i];a->word[i]=(uint32_t)(n/10);remainder=n%10;}pd_trim(a);return (unsigned)remainder;}

static int __attribute__((noinline)) pd_scan(const char *text,double *out,unsigned limit)
{
 if(!text||!out)return 0;
 if(!*text)return -1;
 size_t length=0;while(length<=limit&&text[length])length++;if(length>limit)return 0;
 const char *p=text;bool negative=*p=='-';if(negative)p++;
 pd_big numerator,denominator;pd_set(&numerator,0);
 unsigned fraction=0,digits=0;bool dot=false,nonzero=false;
 /* JSON integer grammar (no plus, leading zero, whitespace, or prefix match). */
 if(*p<'0'||*p>'9')return 0;
 bool leading_zero=*p=='0';
 for(;;){
  unsigned c=(unsigned char)*p;
  if(c>='0'&&c<='9'){
   if(!dot&&leading_zero&&digits)return 0;
   if(dot)fraction++;
   if(c!='0'||nonzero){nonzero=true;digits++;if(!pd_mul(&numerator,10,c-'0'))return 0;}
   else if(!dot)digits++;
   p++;continue;
  }
  if(c=='.'&&!dot){dot=true;p++;if(*p<'0'||*p>'9')return 0;continue;}
  break;
 }
 int exponent=0;
 if(*p=='e'||*p=='E'){
  p++;bool minus=*p=='-';if(*p=='-'||*p=='+')p++;
  if(*p<'0'||*p>'9')return 0;
  while(*p>='0'&&*p<='9'){if(exponent<10000)exponent=exponent*10+(*p-'0');p++;}
  if(minus)exponent=-exponent;
 }
 if(*p)return 0;
 uint64_t bits=negative?UINT64_C(1)<<63:0;
 if(!numerator.used){memcpy(out,&bits,8);return 1;}
 /* Determine actual significant decimal length; the grammar's digits counter
  * includes an integer leading zero, so derive it from the token instead. */
 unsigned significant=0;nonzero=false;
 for(const char *q=text+negative;q<p&&*q!='e'&&*q!='E';q++)if(*q>='0'&&*q<='9'){if(*q!='0'||nonzero){nonzero=true;significant++;}}
 exponent-=(int)fraction;
 int decimal_order=(int)significant+exponent-1;
 if(decimal_order>308)return 0;
 if(decimal_order< -324){memcpy(out,&bits,8);return 1;}
 pd_set(&denominator,1);
 if(exponent>=0){for(int i=0;i<exponent;i++)if(!pd_mul(&numerator,10,0))return 0;}
 else for(int i=0;i< -exponent;i++)if(!pd_mul(&denominator,10,0))return 0;
 int binary_order=(int)pd_bits(&numerator)-(int)pd_bits(&denominator);
 if(binary_order>=0){if(pd_cmp(&numerator,&denominator,(unsigned)binary_order)<0)binary_order--;}
 else if(pd_cmp(&denominator,&numerator,(unsigned)-binary_order)>0)binary_order--;
 int shift=binary_order< -1022?1074:52-binary_order;
 if(shift>=0){if(!pd_shift(&numerator,(unsigned)shift))return 0;}
 else if(!pd_shift(&denominator,(unsigned)-shift))return 0;
 int quotient_bits=(int)pd_bits(&numerator)-(int)pd_bits(&denominator);
 if(quotient_bits>53)return 0;
 uint64_t quotient=0;
 for(int i=quotient_bits;i>=0;i--)if(pd_cmp(&numerator,&denominator,(unsigned)i)>=0){pd_sub(&numerator,&denominator,(unsigned)i);quotient|=UINT64_C(1)<<i;}
 int half=pd_cmp(&denominator,&numerator,1);
 if(half<0||(half==0&&(quotient&1)))quotient++;
 if(binary_order>= -1022){
  if(quotient==(UINT64_C(1)<<53)){quotient>>=1;binary_order++;}
  if(binary_order>1023)return 0;
  bits|=(uint64_t)(binary_order+1023)<<52;
  bits|=quotient&((UINT64_C(1)<<52)-1);
 }else bits|=quotient;
 memcpy(out,&bits,8);return 1;
}

int ph_decimal_scan(const char *text,double *out){return pd_scan(text,out,349);}

static int __attribute__((noinline)) pd_format(char *out,unsigned capacity,double value,unsigned precision)
{
 uint64_t bits;memcpy(&bits,&value,8);bool negative=(bits>>63)!=0;
 unsigned encoded=(unsigned)(bits>>52)&2047;uint64_t mantissa=bits&((UINT64_C(1)<<52)-1);
 char result[32],*p=result;
 if(encoded==2047){if(!mantissa&&negative)*p++='-';memcpy(p,mantissa?"nan":"inf",3);p+=3;}
 else if(!encoded&&!mantissa){if(negative)*p++='-';*p++='0';}
 else {
  if(negative)*p++='-';
  int binary=encoded?(int)encoded-1023-52:-1074;
  if(encoded)mantissa|=UINT64_C(1)<<52;
  while(!(mantissa&1)){mantissa>>=1;binary++;}
  pd_big integer;pd_set(&integer,mantissa);
  unsigned scale=0;
  if(binary>=0){if(!pd_shift(&integer,(unsigned)binary))return -1;}
  else {scale=(unsigned)-binary;for(unsigned i=0;i<scale;i++)if(!pd_mul(&integer,5,0))return -1;}
  char exact[800];unsigned count=0;
  while(integer.used){if(count==sizeof(exact))return -1;exact[count++]=(char)('0'+pd_div10(&integer));}
  int exponent=(int)count-(int)scale;
  /* Exact digits are reversed. Round the first precision significant digits
   * once, using guard/sticky and an even final digit, before choosing layout. */
  char digits[9];unsigned kept=count<precision?count:precision;
  for(unsigned i=0;i<kept;i++)digits[i]=exact[count-1-i];
  if(count>precision){
   unsigned remaining=count-precision;char guard=exact[remaining-1];bool sticky=false;
   for(unsigned i=0;i+1<remaining;i++)sticky|=exact[i]!='0';
   if(guard>'5'||(guard=='5'&&(sticky||((digits[kept-1]-'0')&1)))){
    unsigned i=kept;while(i&&digits[i-1]=='9')digits[--i]='0';
    if(i)digits[i-1]++;else {digits[0]='1';exponent++;}
   }
  }
  while(kept>1&&digits[kept-1]=='0')kept--;
  if(exponent<= -4||exponent>(int)precision){
   *p++=digits[0];if(kept>1){*p++='.';memcpy(p,digits+1,kept-1);p+=kept-1;}
   exponent--;*p++='e';*p++=exponent<0?'-':'+';
   unsigned magnitude=(unsigned)(exponent<0?-exponent:exponent);char reversed[4];unsigned used=0;
   do {reversed[used++]=(char)('0'+magnitude%10);magnitude/=10;}while(magnitude);
   if(used==1)*p++='0';
   while(used)*p++=reversed[--used];
  }else if(exponent<=0){*p++='0';*p++='.';for(int i=0;i< -exponent;i++)*p++='0';memcpy(p,digits,kept);p+=kept;}
  else {for(int i=0;i<exponent;i++)*p++=(unsigned)i<kept?digits[i]:'0';if(exponent<(int)kept){*p++='.';memcpy(p,digits+exponent,kept-(unsigned)exponent);p+=kept-(unsigned)exponent;}}
 }
 unsigned length=(unsigned)(p-result);if(!out||length>=capacity)return -1;memcpy(out,result,length);out[length]=0;return (int)length;
}
int ph_decimal_format(char *out,unsigned capacity,double n){return pd_format(out,capacity,n,6);}
int ph_decimal_format_float32(char *out,unsigned capacity,float n){return pd_format(out,capacity,(double)n,9);}
