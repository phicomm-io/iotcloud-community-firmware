/* Exercise the actual optional cache under allocation failure and OTA polling. */
#include "../src/device.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
static bool fail_cache;
static unsigned attempts,allocations,releases;
static void *cache;
void *__real_malloc(size_t);
void __real_free(void *);
void *__wrap_malloc(size_t n) {
    if(n!=PH_STATUS_CACHE_MAX)return __real_malloc(n);
    ++attempts;if(fail_cache)return NULL;
    assert(!cache);cache=__real_malloc(n);if(cache)++allocations;return cache;
}
void __wrap_free(void *p) {
    if(p&&p==cache){++releases;cache=NULL;}
    __real_free(p);
}
static uint32_t tick(void *p){(void)p;return 10000;}
static bool save(void *p,const ph_settings *s){(void)p;(void)s;return true;}
static void relay(void *p,bool on){(void)p;(void)on;}
int main(void) {
    ph_device d;ph_device_ops ops={0};ops.now=tick;ops.save=save;ops.relay=relay;
    assert(ph_device_init(&d,&ops,NULL,"aabbccddeeff","cache-test",123));
    unsigned char expected[2][PH_OUTPUT_MAX],actual[PH_OUTPUT_MAX];size_t sizes[2];
    for(unsigned f=1;f<=2;++f){sizes[f-1]=ph_query(&d,"status.get",f,expected[f-1],sizeof(actual));assert(sizes[f-1]);}
    assert(allocations==1&&attempts==1&&d.cache_builds==1);
    for(unsigned cycle=0;cycle<100;++cycle){
        ph_device_set_maintenance(&d,true);
        assert(!cache&&!d.status_cache&&!d.status_size&&allocations==releases);
        unsigned before=attempts;
        for(unsigned poll=0;poll<10;++poll)for(unsigned f=1;f<=2;++f){
            size_t n=ph_query(&d,"status.get",f,actual,sizeof(actual));
            assert(n==sizes[f-1]&&!memcmp(actual,expected[f-1],n));
        }
        assert(attempts==before); /* No polling can reclaim OTA's memory. */
        ph_device_set_maintenance(&d,false);fail_cache=true;
        for(unsigned f=1;f<=2;++f){
            size_t n=ph_query(&d,"status.get",f,actual,sizeof(actual));
            assert(n==sizes[f-1]&&!memcmp(actual,expected[f-1],n));
            assert(!d.status_cache&&!d.status_size);
        }
        fail_cache=false;
        assert(ph_query(&d,"status.get",PH_WIRE_MSGPACK,actual,sizeof(actual))==sizes[0]);
        assert(d.status_cache&&allocations==releases+1);
    }
    ph_status_cache_release(&d);ph_status_cache_release(&d);
    assert(!cache&&allocations==releases);
    assert(ph_device_init(&d,&ops,NULL,"aabbccddeeff","cache-test",456));
    assert(ph_query(&d,"status.get",PH_WIRE_CBOR,actual,sizeof(actual)));
    unsigned before=attempts;
    ph_status_cache_suspend(&d,true);
    assert(!d.maintenance&&d.cache_suspended&&!d.status_cache);
    ph_device_set_maintenance(&d,false);
    for(unsigned poll=0;poll<20;++poll)assert(ph_query(&d,"status.get",PH_WIRE_MSGPACK,actual,sizeof(actual)));
    assert(attempts==before); /* HTTP/OTA completion cannot reclaim pending MQTT memory. */
    ph_status_cache_budget(&d,8192u+PH_STATUS_CACHE_MAX-1,8192u);
    assert(d.cache_suspended);
    ph_status_cache_budget(&d,8192u+PH_STATUS_CACHE_MAX,8192u);
    assert(!d.cache_suspended);
    assert(ph_query(&d,"status.get",PH_WIRE_MSGPACK,actual,sizeof(actual)));
    assert(attempts==before+1&&d.status_cache);
    ph_status_cache_budget(&d,8192u,8192u);assert(!d.cache_suspended&&d.status_cache);
    ph_status_cache_budget(&d,8191u,8192u);assert(d.cache_suspended&&!d.status_cache);
    ph_status_cache_release(&d);assert(allocations==releases);
    puts("PASS: cached/uncached MessagePack and CBOR, 100 OTA reclamation cycles, OOM fallback, MQTT suspension/budget boundaries and reinitialization");
    return 0;
}
