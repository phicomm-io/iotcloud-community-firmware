/* The production SDK SHA256 path must never allocate, including long inputs. */
#include "../src/ph_sha256.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned allocations;
void *__wrap_malloc(size_t n){(void)n;allocations++;return NULL;}
void *__wrap_realloc(void *p,size_t n){(void)p;(void)n;allocations++;return NULL;}
void __wrap_free(void *p){(void)p;allocations++;}
static unsigned digit(char c){return c<='9'?(unsigned)(c-'0'):(unsigned)(c-'a'+10);}
int main(void){
 uint8_t out[32],bytes[4096];char line[8194];static unsigned char million[1000000];
 assert(!PH_BL_Sha256(NULL,1,out)&&!PH_BL_Sha256("",0,NULL));
#if SIZE_MAX>UINT32_MAX
 assert(!PH_BL_Sha256("",(size_t)UINT32_MAX+1,out));
#endif
 while(fgets(line,sizeof(line),stdin)){
  size_t n=strcspn(line,"\r\n");line[n]=0;const void *input=bytes;
  if(!strcmp(line,"million")){memset(million,'a',sizeof(million));input=million;n=sizeof(million);}
  else{assert(!(n&1)&&n/2<=sizeof(bytes));n/=2;for(size_t i=0;i<n;i++)bytes[i]=(uint8_t)(digit(line[2*i])*16+digit(line[2*i+1]));}
  assert(PH_BL_Sha256(input,n,out)&&!allocations);
  for(unsigned i=0;i<32;i++)printf("%02x",out[i]);putchar('\n');fflush(stdout);
 }
 return 0;
}
