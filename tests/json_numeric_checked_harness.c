/* Executes the production JSON library and private numeric port with pinned
 * ARM libc_nano. QEMU is a userspace conversion/ownership test, not a DUT. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include <errno.h>
#include <reent.h>
#include "json.h"
#include "json_number_checked.h"
#include "json_numeric/context.h"
#include "json_heap.h"
#include "json_owned.h"
static union { double alignment; unsigned char bytes[8192]; } arena[128];
static size_t sizes[128], live, calls, fail_at, failures, peak;
static unsigned lock_depth;
extern struct _Bigint *json_num_Balloc(struct json_num_context *, int);
static void output(const char *s) {
    size_t length = strlen(s);
    register int r0 __asm__("r0") = 1;
    register const char *r1 __asm__("r1") = s;
    register size_t r2 __asm__("r2") = length;
    register int r7 __asm__("r7") = 4;
    __asm__ volatile("svc 0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r7) : "memory");
}
static void integer(size_t n) {
    char b[24]; unsigned i=sizeof(b)-1; b[i]=0;
    do { b[--i]=(char)('0'+n%10); n/=10; } while(n); output(b+i);
}
void _exit(int code) {
    register int r0 __asm__("r0") = code;
    register int r7 __asm__("r7") = 1;
    __asm__ volatile("svc 0" :: "r"(r0), "r"(r7) : "memory"); for(;;) {}
}
static void checked(int good, unsigned line) {
    if (!good) { output("FAIL line="); integer(line); output("\n"); _exit(1); }
}
#define CHECK(c) checked(!!(c),__LINE__)
void *__wrap_malloc(size_t n) {
    ++calls; if (fail_at && calls == fail_at) return NULL;
    CHECK(n <= sizeof(arena[0].bytes));
    for (unsigned i=0; i<128; ++i) if (!sizes[i]) {
        sizes[i]=n?n:1; live+=sizes[i]; if(live>peak) peak=live;
        memset(arena[i].bytes,0xa5,sizeof(arena[i].bytes)); return arena[i].bytes;
    }
    CHECK(0); return NULL;
}
void __wrap_free(void *p) {
    if (!p) return;
    for(unsigned i=0;i<128;++i) if(p==arena[i].bytes) {
        CHECK(sizes[i]); live-=sizes[i]; sizes[i]=0;
        memset(arena[i].bytes,0xdd,sizeof(arena[i].bytes)); return;
    }
    CHECK(0);
}
void *__wrap_calloc(size_t n,size_t s) {
    CHECK(!s || n<=SIZE_MAX/s); void *p=__wrap_malloc(n*s);
    if(p) memset(p,0,n*s); return p;
}
void *__wrap_realloc(void *p,size_t n) {
    if(!p) return __wrap_malloc(n);
    for(unsigned i=0;i<128;++i) if(p==arena[i].bytes) {
        size_t old=sizes[i]; CHECK(old);
        void *q=__wrap_malloc(n); if(!q) return NULL;
        memcpy(q,p,old<n?old:n); __wrap_free(p); return q;
    }
    CHECK(0); return NULL;
}
void *__wrap__malloc_r(struct _reent *r,size_t n) { (void)r; return __wrap_malloc(n); }
void *__wrap__calloc_r(struct _reent *r,size_t n,size_t s) { (void)r; return __wrap_calloc(n,s); }
void *__wrap__realloc_r(struct _reent *r,void *p,size_t n) { (void)r; return __wrap_realloc(p,n); }
void __wrap__free_r(struct _reent *r,void *p) { (void)r; __wrap_free(p); }
void *_sbrk(ptrdiff_t n) { (void)n; CHECK(0); return NULL; }
void json_heap_lock(void) { CHECK(!lock_depth); ++lock_depth; }
void json_heap_unlock(void) { CHECK(lock_depth==1); --lock_depth; }
void json_heap_failure(unsigned reason) { CHECK(reason); CHECK(lock_depth==1); ++failures; }
void json_numeric_test_token_bound(const char *token, unsigned capacity) {
    for(unsigned i=0;i<capacity;++i) if(!token[i]) return;
    CHECK(0);
}
static void native_cleanup(struct _reent *c) {
    if(c->_mp) {
        struct _Bigint *p=_REENT_MP_P5S(c); _REENT_MP_P5S(c)=NULL;
        while(p) { struct _Bigint *next=p->_next; __wrap_free(p); p=next; }
    }
    _reclaim_reent(c);
}
static int native_scan(const char *text,double *d) {
    struct _reent c; _REENT_INIT_PTR(&c);
    int n=_sscanf_r(&c,text,"%lf",d); native_cleanup(&c); return n;
}
static int native_format(char *b,unsigned cap,double d,int fixed,unsigned precision) {
    struct _reent c; _REENT_INIT_PTR(&c);
    int n=fixed?_snprintf_r(&c,b,cap,"%.*f",precision,d):_snprintf_r(&c,b,cap,"%g",d);
    native_cleanup(&c); return n;
}
static int same(double a,double b) { return !memcmp(&a,&b,sizeof(a)); }
static void scan_parity(const char *text) {
    double a=123,b=123; int n=native_scan(text,&a);
    int m=json_number_scan(text,&b);
    if(n!=m || !same(a,b)) { output("scan mismatch "); output(text); output(" native=");integer(n+3);output(" private=");integer(m+3);output("\n"); CHECK(0); }
    CHECK(!live);
}
static void format_parity(double d) {
    char native[340],private[340];
    int a=native_format(native,sizeof(native),d,0,0);
    int b=json_number_format(private,sizeof(private),d);
    if(a!=b || strcmp(native,private)) { output("format mismatch native=");output(native);output(" private=");output(private);output("\n");CHECK(0); }
    for(unsigned precision=0;precision<=6;++precision) {
        a=native_format(native,sizeof(native),d,1,precision);
        b=json_number_format_fixed(private,sizeof(private),d,precision);
        if(a!=b || strcmp(native,private)) { output("fixed mismatch precision=");integer(precision);output(" native=");output(native);output(" private=");output(private);output("\n");CHECK(0); }
    }
    CHECK(!live);
}
static void fail_scan(const char *text) {
    calls=0; double d=42; CHECK(json_number_scan(text,&d)==1); size_t count=calls;
    for(size_t ordinal=1;ordinal<=count;++ordinal) {
        calls=0;fail_at=ordinal;failures=0;d=42;
        CHECK(json_number_scan(text,&d)==-2); CHECK(d==42); CHECK(failures); CHECK(!live);
        fail_at=0;CHECK(json_number_scan(text,&d)==1);CHECK(!live);
    }
}
static void fail_format(double d,int fixed,unsigned precision) {
    char b[340]; calls=0;
    CHECK((fixed?json_number_format_fixed(b,sizeof(b),d,precision):json_number_format(b,sizeof(b),d))>0);
    size_t count=calls;
    for(size_t ordinal=1;ordinal<=count;++ordinal) {
        calls=0;fail_at=ordinal;failures=0;memset(b,0x5a,sizeof(b));
        CHECK((fixed?json_number_format_fixed(b,sizeof(b),d,precision):json_number_format(b,sizeof(b),d))==-1);
        for(unsigned i=0;i<sizeof(b);++i) CHECK(b[i]==0x5a);
        CHECK(failures);CHECK(!live);fail_at=0;
    }
}
static void full_json(void) {
    const char *text="[0.12345678901234567,1e100,-1e-300,999999.5]";
    calls=0;struct json_object *o=json_tokener_parse(text);CHECK(o);CHECK(json_object_to_json_string(o));json_object_put(o);CHECK(!live);
    size_t count=calls;
    for(size_t ordinal=1;ordinal<=count;++ordinal) {
        calls=0;fail_at=ordinal;failures=0;
        struct json_tokener *t=json_tokener_new();
        if(t) {
            o=json_tokener_parse_ex(t,text,(int)strlen(text)+1);
            if(!o) CHECK(t->err==json_tokener_error_memory);
            else { const char *encoded=json_object_to_json_string(o); if(!encoded) CHECK(failures);json_object_put(o); }
            json_tokener_free(t);
        }
        CHECK(failures);fail_at=0;CHECK(!live);
    }
    o=json_object_new_string("0.12345678901234567");CHECK(o);size_t retained=live;
    calls=0;CHECK(json_object_get_double(o)>0);count=calls;
    for(size_t ordinal=1;ordinal<=count;++ordinal) {
        calls=0;fail_at=ordinal;failures=0;errno=0;
        CHECK(json_object_get_double(o)==0);CHECK(failures);CHECK(errno==ENOMEM);CHECK(live==retained);fail_at=0;
    }
    json_object_put(o);CHECK(!live);
    o=json_object_new_double(0.12345678901234567);CHECK(o);
    calls=0;enum json_owned_error error;
    struct printbuf *wire=json_owned_to_wire_result(o,&error);CHECK(wire);printbuf_free(wire);CHECK(!live);count=calls;
    for(size_t ordinal=1;ordinal<=count;++ordinal) {
        o=json_object_new_double(0.12345678901234567);CHECK(o);
        calls=0;fail_at=ordinal;failures=0;
        wire=json_owned_to_wire_result(o,&error);
        CHECK(!wire);CHECK(error==JSON_OWNED_RESOURCE);CHECK(failures);CHECK(!live);fail_at=0;
    }
}
void _start(void) {
    output("START numeric\n");
    const char *texts[]={""," ","abc","+","-.","1e","1e+","1e-","0x123","nan(payload)","nan","-NaN","inf","-infinity","infinite","n","na","in","0e0","-0.000e-000","  +12.5junk","1e99999","1e-99999","1.7976931348623157e308","2.2250738585072014e-308","4.9406564584124654e-324","0.12345678901234567"};
    for(unsigned i=0;i<sizeof(texts)/sizeof(*texts);++i) scan_parity(texts[i]);
    const double boundaries[]={0.,-0.,1.,-1.,999999.5,999999.4,0.0001,0.00009999995,DBL_MAX,DBL_MIN,4.9406564584124654e-324,INFINITY,-INFINITY,NAN,1.25,2.5,0.00000049,0.0000005};
    for(unsigned i=0;i<sizeof(boundaries)/sizeof(*boundaries);++i) {
        format_parity(boundaries[i]);fail_format(boundaries[i],0,0);
        for(unsigned p=0;p<=6;++p)fail_format(boundaries[i],1,p);
    }
    uint64_t state=UINT64_C(0x345abc987123cdef);
    for(unsigned i=0;i<4000;++i) {
        state^=state<<13;state^=state>>7;state^=state<<17;
        double d;memcpy(&d,&state,sizeof(d));format_parity(d);
        char b[64];struct _reent c;_REENT_INIT_PTR(&c);
        _snprintf_r(&c,b,sizeof(b),"%.17g",d);native_cleanup(&c);scan_parity(b);
    }
    char long_number[4096];
    memset(long_number,'0',sizeof(long_number));memcpy(long_number+4000,"123.004500e-10",14);scan_parity(long_number);fail_scan(long_number);
    memset(long_number,'7',4090);memcpy(long_number+4090,"e-400",6);scan_parity(long_number);fail_scan(long_number);
    for(unsigned n=340;n<360;++n) {
        memset(long_number,'3',n);long_number[1]='.';strcpy(long_number+n,"e-100");scan_parity(long_number);fail_scan(long_number);
    }
    /* Regression for an inherited normalization overrun: the decimal token
     * fills the native buffer, then e-999999999 adjusts to e-1000000000.
     * The checked frontend must terminate inside its original350-byte limit. */
    memcpy(long_number,"0.0",3);memset(long_number+3,'1',337);strcpy(long_number+340,"e-999999999");
    double normalized=42;CHECK(json_number_scan(long_number,&normalized)==1);CHECK(normalized==0);CHECK(!live);
    /* Do not call the native oracle on its signed-overflow exponent path. */
    normalized=42;CHECK(json_number_scan("1e9999999999",&normalized)==1);CHECK(isinf(normalized)&&normalized>0);CHECK(!live);
    normalized=42;CHECK(json_number_scan("-1e-9999999999",&normalized)==1);CHECK(normalized==0&&signbit(normalized));CHECK(!live);
    fail_scan("0.12345678901234567");fail_scan("1e100");fail_scan("1e-300");full_json();
    struct json_num_context c={0};
    CHECK(!json_num_Balloc(&c,30));CHECK(c.failed);CHECK(!c.allocations);json_num_cleanup(&c);
    memset(&c,0,sizeof(c));CHECK(!json_num_Balloc(&c,-1));CHECK(c.failed);json_num_cleanup(&c);
    memset(&c,0,sizeof(c));CHECK(!json_num_Balloc(&c,31));CHECK(c.failed);json_num_cleanup(&c);
    memset(&c,0,sizeof(c));
    json_heap_stats_t before,after;json_heap_snapshot(&before);
    CHECK(!json_num_calloc(&c,SIZE_MAX,2));json_heap_snapshot(&after);CHECK(after.guard_denied==before.guard_denied+1);CHECK(c.failed);json_num_cleanup(&c);json_num_cleanup(&c);CHECK(!live);
    memset(&c,0,sizeof(c));CHECK(!json_num_calloc(&c,1,JSON_NUMERIC_SCRATCH_LIMIT));CHECK(c.failed);json_num_cleanup(&c);
    memset(&c,0,sizeof(c));for(unsigned i=0;i<JSON_NUMERIC_BLOCK_LIMIT;++i)CHECK(json_num_calloc(&c,1,1));CHECK(!json_num_calloc(&c,1,1));json_num_cleanup(&c);CHECK(!live);
    char untouched[8];memset(untouched,'Z',sizeof(untouched));CHECK(json_number_format_fixed(untouched,sizeof(untouched),DBL_MAX,6)<0);for(unsigned i=0;i<sizeof(untouched);++i)CHECK(untouched[i]=='Z');
    CHECK(!lock_depth);
    output("PASS numeric parity, fullJSON, fail-each allocation, cleanup; peak-live-bytes=");integer(peak);output("\n");_exit(0);
}
