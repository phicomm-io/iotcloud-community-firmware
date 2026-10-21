/* Linux userspace syscalls for pinned ARM/nano ABI tests; no MiCO emulation. */
#include <stddef.h>
static unsigned char arena[256*1024];static size_t used;
void *_sbrk(ptrdiff_t n){if(n<0 || n>sizeof(arena)-used)return (void*)-1;void*p=arena+used;used+=n;return p;}
void _exit(int c){register int r0 __asm__("r0")=c;register int r7 __asm__("r7")=1;__asm__ volatile("svc 0"::"r"(r0),"r"(r7):"memory");for(;;){}}
int _write(int fd,const void*p,int n){register int r0 __asm__("r0")=fd;register const void*r1 __asm__("r1")=p;register int r2 __asm__("r2")=n;register int r7 __asm__("r7")=4;__asm__ volatile("svc 0":"+r"(r0):"r"(r1),"r"(r2),"r"(r7):"memory");return r0;}

int main(void);void _start(void){_exit(main());}
