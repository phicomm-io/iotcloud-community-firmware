"""Actual dispatcher failure boundaries; host only, no sockets or peripherals.

Unscoped calls retain conservative shedding; an owner-bound lease ignores
unrelated failures without allowing a partial response from its own failure.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from tests.sdk_json import copy_patched_json

ROOT = Path(__file__).resolve().parents[1]

FIXTURE = r'''
#include "mico.h"
#include "common/service.c"
#include "common/json_owned.h"
#include "common/httpd/product.h"
#include "json_object_private.h"
uint32_t test_now;
static mico_thread_t foreground;
static unsigned allocation, fail_at, builds;
static unsigned foreign_at, foreign_alloc;
static bool foreign_pending;
static bool foreign_failure, own_missing_model;
static bool unknown_http_owner;
void *ph_http_json_owner(void) { return unknown_http_owner ? NULL : &foreground; }

/* Only this disposable JSON copy redirects the allocation permit callback. */
int test_scope_permit(size_t n) {
    if(foreign_at && ++foreign_alloc==foreign_at)foreign_pending=true;
    return json_heap_permit(n) && (!fail_at || ++allocation != fail_at);
}
static void *unrelated_failure(void *unused) {
    (void)unused;
    /* The foreground owns the single graph lease; a failed independent heap
     * operation does not require (and cannot acquire) that lease. */
    assert(!service_json_claim());
    /* Deterministic real allocator overflow rejection, with no partial graph. */
    assert(!json_heap_calloc(SIZE_MAX, 2));
    return NULL;
}
/* Deterministically schedule another allocator user between two real parser
 * allocations, after releasing the actual allocator lock (never inside it). */
void test_after_unlock(void) {
    if(!foreign_pending)return;
    foreign_pending=false;foreign_at=0;
    pthread_t worker;assert(!pthread_create(&worker,NULL,unrelated_failure,NULL));
    assert(!pthread_join(worker,NULL));
}
json_object *automation_command(const char *a,json_object *p,const char **c,bool *h)
{ (void)a;(void)p;(void)c;*h=false;return NULL; }
json_object *service_product_capabilities(void) { return json_object_new_object(); }
json_object *service_product(const char *a,json_object *p,const char **c) {
    (void)a;(void)p;*c="ok";builds++;
    json_object *o=json_object_new_object();
    if(own_missing_model) {
        /* One required child fails, then allocations recover before the final
         * info.get envelope is built. Its temporary status graph is discarded. */
        fail_at=1;allocation=0;
        json_build_object_add(o,"model",json_object_new_string("a1"));
        fail_at=0;
    } else json_build_object_add(o,"model",json_object_new_string("a1"));
    json_build_object_add(o,"device_id",json_object_new_string("scope-fixture"));
    json_build_object_add(o,"version",json_object_new_string("host"));
    if(foreign_failure) {
        /* Both healthy required fields and graph exist before the other thread
         * fails; join puts that failure strictly inside service_command(). */
        assert(o && !o->_failed);
        pthread_t worker;
        assert(!pthread_create(&worker,NULL,unrelated_failure,NULL));
        assert(!pthread_join(worker,NULL));
        assert(!o->_failed);
    }
    return o;
}
static json_object *request(const char *action) {
    char text[128];snprintf(text,sizeof(text),"{\"action\":\"%s\",\"params\":{}}",action);
    json_object *q=service_parse(text,strlen(text));assert(q);return q;
}
static void empty(void) {
    json_heap_stats_t s;json_heap_snapshot(&s);assert(!s.live_blocks);
}
static void healthy(json_object *r) {
    assert(r && !strcmp(json_object_get_string(json_object_object_get(r,"code")),"ok"));
    json_object *data=json_object_object_get(r,"data");
    assert(!strcmp(json_object_get_string(json_object_object_get(data,"model")),"a1"));
    struct printbuf *wire=json_owned_to_wire(r);assert(wire);printbuf_free(wire);
}
static void foreign(void) {
    unsigned token=service_json_claim();assert(token);
    foreign_failure=true;
    json_object *q=request("status.get");
    uint64_t before=service_memory_failures();
    /* The actual inner dispatcher produces a fully encodable response. */
    healthy(service_command_execute(q));
    assert(service_memory_failures()>before);
    assert(!service_command(q));
    json_object_put(q);
    assert(!service_query("status.get"));
    foreign_failure=false;healthy(service_query("status.get"));empty();
    service_json_release(token);
    puts("REPRODUCED: real foreign-thread failure drops intact command/query; recovery clean");
}
static void scoped_foreign(void) {
    unsigned token=service_json_claim();assert(token);
    assert(service_json_scope_begin(token,&foreground));
    foreign_failure=true;json_object *q=request("status.get");
    uint64_t before=service_memory_failures();
    healthy(service_command(q));json_object_put(q);
    healthy(service_query("status.get"));
    assert(service_memory_failures()>before && !service_json_scope_failed(token));
    assert(!service_json_scope_end(token));service_json_release(token);empty();
    puts("FIXED: owner-bound command/query ignore foreign failures; global diagnostics retained");
}
static void discarded(void) {
    unsigned token=service_json_claim();assert(token);
    assert(service_json_scope_begin(token,&foreground));
    own_missing_model=true;json_object *q=request("info.get");
    json_object *r=service_command_execute(q);assert(r && !r->_failed);
    json_object *data=json_object_object_get(r,"data");assert(data && !data->_failed);
    assert(!json_object_object_get(data,"model"));
    /* A final-graph-only check would serialize this incomplete success. */
    struct printbuf *wire=json_owned_to_wire(r);assert(wire);printbuf_free(wire);
    assert(!service_command(q));json_object_put(q);
    assert(service_json_scope_end(token));service_json_release(token);
    own_missing_model=false;healthy(service_query("info.get"));empty();
    puts("OWN FAILURE: discarded failed status graph escapes final graph flag; wrapper rejects");
}
static void sweep(void) {
    unsigned failures=0;
    for(unsigned nth=1;nth<1000;nth++) {
        unsigned token=service_json_claim();assert(token);
        assert(service_json_scope_begin(token,&foreground));
        allocation=0;fail_at=nth;
        json_object *r=service_query("status.get");
        unsigned calls=allocation;fail_at=0;
        unsigned failure=service_json_scope_end(token);service_json_release(token);
        if(r) { assert(calls<nth && !failure);healthy(r);empty();break; }
        assert(calls>=nth && failure);failures++;empty();
    }
    assert(failures>20 && failures<999);
    /* Parse allocation failures cannot return a truncated command, either. */
    const char *text="{\"action\":\"fan.set\",\"params\":{\"on\":true,\"speed\":20}}";
    unsigned parse_failures=0;
    for(unsigned nth=1;nth<1000;nth++) {
        unsigned token=service_json_claim();assert(token);
        assert(service_json_scope_begin(token,&foreground));
        allocation=0;fail_at=nth;json_object *q=service_parse(text,strlen(text));
        unsigned calls=allocation;fail_at=0;
        unsigned failure=service_json_scope_end(token);service_json_release(token);
        if(q) { assert(calls<nth && !failure);json_object_put(q);empty();break; }
        assert(calls>=nth && failure);parse_failures++;empty();
    }
    assert(parse_failures>10 && parse_failures<999);
    healthy(service_query("status.get"));empty();
    printf("OWN FAILURE: %u query + %u parse allocation points rejected, no partial success/leak\n",failures,parse_failures);
}
static unsigned active_token;
static void *wrong_owner(void *unused) {
    (void)unused;mico_thread_t self=pthread_self();
    assert(!service_json_scope_begin(active_token,&self));
    assert(service_json_scope_failed(active_token)==4);
    assert(service_json_scope_end(active_token)==4);
    assert(!service_json_scope_begin(active_token,&foreground));
    assert(!json_heap_calloc(SIZE_MAX,2));return NULL;
}
static void boundaries(void) {
    foreground=pthread_self();
    assert(!service_json_scope_begin(0,&foreground));
    assert(!service_json_scope_begin(123,&foreground));
    active_token=service_json_claim();assert(active_token);
    assert(!service_json_scope_begin(active_token,NULL));
    assert(service_json_scope_begin(active_token,&foreground));
    assert(!service_json_scope_begin(active_token,&foreground)); /* No nested reset. */
    assert(!service_json_scope_begin(active_token+1,&foreground));
    assert(service_json_scope_end(active_token+1)==4);
    pthread_t worker;assert(!pthread_create(&worker,NULL,wrong_owner,NULL));
    assert(!pthread_join(worker,NULL));assert(!service_json_scope_failed(active_token));
    assert(!json_heap_calloc(SIZE_MAX,2));assert(service_json_scope_failed(active_token)&4);
    assert(!service_json_scope_begin(active_token,&foreground)); /* Sticky failure. */
    service_json_release(active_token); /* Cleanup even without explicit end. */
    unsigned heir=service_json_claim();assert(heir && heir!=active_token);
    assert(service_json_scope_begin(heir,&foreground));
    service_json_release(active_token);assert(service_json_scope_end(active_token)==4);
    assert(!service_json_scope_failed(heir));assert(!service_json_scope_end(heir));
    service_json_release(heir);empty();
    puts("lease scope: stale/nested/wrong-owner/release cleanup + overflow attribution PASS");
}
static void http(void) {
    const char *text="{\"action\":\"status.get\",\"params\":{}}";
    ph_http_request req={0};req.length=strlen(text);
    ph_http_exchange x={0};x.request=&req;ph_http_product p;
    ph_http_product_init(&p,&x);
    assert(ph_http_claim(&p,&x,req.length,true)==PH_HTTP_READ_BODY);
    assert(!p.json_scoped && !json_scope.token); /* No scope over body socket wait. */
    pthread_t worker;assert(!pthread_create(&worker,NULL,unrelated_failure,NULL));
    assert(!pthread_join(worker,NULL));
    assert(ph_http_body_copy(&p,text,req.length));
    foreign_at=1;foreign_alloc=0;uint64_t failures=service_memory_failures();
    json_object *q=ph_http_body_json(&p,&x);assert(q && p.json_scoped);
    assert(!foreign_at && service_memory_failures()>failures && !ph_http_graph_failed(&p));
    foreign_failure=true;
    assert(!ph_http_graph_failed(&p));json_object *r=service_command(q);assert(r);
    json_object_put(q);ph_http_json(&p,&x,r);assert(x.response.status==200);
    assert(!p.json_scoped && !json_scope.token);ph_http_product_release(&p);empty();
    foreign_failure=false;
    /* Body allocation failure is handled before any scope begins. */
    ph_http_product_init(&p,&x);allocation=0;fail_at=1;
    assert(ph_http_claim(&p,&x,req.length,true)==PH_HTTP_REPLY);fail_at=0;
    assert(!p.body && !p.json_scoped && x.response.status==503);
    ph_http_product_release(&p);empty();
    /* A parse OOM must not dispatch an invalid/partial command as success. */
    ph_http_product_init(&p,&x);
    assert(ph_http_claim(&p,&x,req.length,true)==PH_HTTP_READ_BODY);
    assert(ph_http_body_copy(&p,text,req.length));allocation=0;fail_at=1;
    q=ph_http_body_json(&p,&x);fail_at=0;assert(!q && ph_http_graph_failed(&p));
    unsigned before=builds;r=!ph_http_graph_failed(&p)?service_command(q):NULL;
    assert(!r && builds==before);ph_http_json(&p,&x,r);assert(x.response.status==503);
    ph_http_product_release(&p);empty();
    /* GET scope also retires before owned encoding, preserving graph checks. */
    ph_http_product_init(&p,&x);assert(ph_http_claim(&p,&x,0,false)==PH_HTTP_REPLY);
    assert(p.json_scoped);r=service_query("status.get");
    json_build_object_add(r,"missing",NULL);ph_http_json(&p,&x,r);
    assert(x.response.status==503 && !json_scope.token);ph_http_product_release(&p);empty();
    /* Unknown owner preserves the original claim-to-dispatch global gate. */
    unknown_http_owner=true;ph_http_product_init(&p,&x);
    assert(ph_http_claim(&p,&x,req.length,true)==PH_HTTP_READ_BODY);
    assert(!pthread_create(&worker,NULL,unrelated_failure,NULL));assert(!pthread_join(worker,NULL));
    assert(ph_http_body_copy(&p,text,req.length));q=ph_http_body_json(&p,&x);
    assert(q && !p.json_scoped && ph_http_graph_failed(&p));json_object_put(q);
    ph_http_product_release(&p);unknown_http_owner=false;empty();
    puts("HTTP query/parse: no scope over body wait/encoding; foreign ignored, own OOM/graph rejected PASS");
}
int main(int argc,char **argv) {
    assert(argc==2);foreground=pthread_self();service_init();assert(ready);
    if(!strcmp(argv[1],"foreign"))foreign();
    else if(!strcmp(argv[1],"scoped_foreign"))scoped_foreign();
    else if(!strcmp(argv[1],"discarded"))discarded();
    else if(!strcmp(argv[1],"boundaries"))boundaries();
    else if(!strcmp(argv[1],"http"))http();
    else { assert(!strcmp(argv[1],"sweep"));sweep(); }
    return 0;
}
'''


class ServiceJsonFailureScope(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        temp = Path(cls.temp.name)
        sdk = Path(os.environ.get('PHICOMM_TEST_JSON_DIR', ROOT/'build/a1-sdk/mico-os/libraries/utilities/json_c'))
        json = copy_patched_json(sdk, temp/'sdk')
        for p in json.glob('*.c'):
            p.write_text(p.read_text().replace('%ld', '%d'))
        heap = json/'json_heap.c'
        heap.write_text('#include <stddef.h>\nint test_scope_permit(size_t);\n' +
                        heap.read_text().replace('if (!json_heap_permit(', 'if (!test_scope_permit('))
        header = (ROOT/'tests/stubs/mico.h').read_text()
        start = header.index('static inline int mico_rtos_init_mutex(')
        end = header.index('static inline uint32_t mico_rtos_get_time(', start)
        header = header[:start] + '''void test_after_unlock(void);
static inline int mico_rtos_init_mutex(mico_mutex_t*m){return pthread_mutex_init(m,NULL);}
static inline void mico_rtos_lock_mutex(mico_mutex_t*m){assert(!pthread_mutex_lock(m));}
static inline void mico_rtos_unlock_mutex(mico_mutex_t*m){assert(!pthread_mutex_unlock(m));test_after_unlock();}
''' + header[end:]
        header = header.replace('typedef unsigned mico_mutex_t;', '#include <pthread.h>\ntypedef pthread_mutex_t mico_mutex_t;')
        header = header.replace('typedef uintptr_t mico_thread_t;', 'typedef pthread_t mico_thread_t;')
        header = header.replace('return thread && *thread==1;', 'return thread && pthread_equal(*thread,pthread_self());')
        (temp/'mico.h').write_text(header)
        (temp/'fixture.c').write_text(FIXTURE)
        bear = ROOT/'build/ota-tls/bearssl-0.6'
        hash_sources = [str(bear/'src'/p) for p in ('hash/sha2small.c', 'codec/dec32be.c', 'codec/enc32be.c', 'codec/enc64be.c')]
        cls.binary = temp/'fixture'
        subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu99', '-g', '-pthread',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie',
                        '-I'+str(bear/'inc'), '-I'+str(bear/'src'), '-I'+str(temp),
                        '-I'+str(ROOT), '-I'+str(ROOT/'tests/stubs'), '-I'+str(json),
                        str(temp/'fixture.c'), str(ROOT/'common/json_owned.c'), str(ROOT/'common/httpd/product.c'),
                        *hash_sources, *map(str, json.glob('*.c')), '-o', str(cls.binary)],
                       check=True, timeout=60)

    def run_case(self, case):
        subprocess.run([str(self.binary), case], check=True, timeout=15)

    def test_current_service_reproduces_foreign_failure_poisoning(self):
        self.run_case('foreign')

    def test_owner_bound_service_ignores_foreign_failure(self):
        self.run_case('scoped_foreign')

    def test_final_graph_flag_misses_failed_discarded_status(self):
        self.run_case('discarded')

    def test_own_query_and_input_allocation_failures_reject_partial_success(self):
        self.run_case('sweep')

    def test_stale_nested_wrong_owner_and_cleanup_boundaries(self):
        self.run_case('boundaries')

    def test_actual_http_query_parse_and_encoder_boundaries(self):
        self.run_case('http')
