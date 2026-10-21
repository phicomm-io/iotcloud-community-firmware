"""Exercise A1 HTTP ownership, send deadlines and JSON lifetime."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "json.h"
#include "printbuf.h"
#include "common/json_build.h"
static const char *response_cors="";
static char output[8192];
static size_t used;
static void *watched[2];
static unsigned freed[2];
static bool fail_realloc, fail_send;
void __real_free(void *);
void *__real_realloc(void *,size_t);
void __wrap_free(void *pointer) {
    for(unsigned i=0;i<2;i++)if(pointer && pointer==watched[i])freed[i]++;
    __real_free(pointer);
}
void *__wrap_realloc(void *pointer,size_t size) {
    if(fail_realloc){fail_realloc=false;return NULL;}
    return __real_realloc(pointer,size);
}
static bool response_send(int fd,const void *header,size_t header_length,const void *data,size_t length) {
    assert(fd==7 && freed[0]==1 && freed[1]==1);
    assert(used+header_length+length<sizeof(output));
    if(fail_send)return false;
    memcpy(output+used,header,header_length);used+=header_length;
    memcpy(output+used,data,length);used+=length;output[used]=0;return true;
}
FUNCTIONS
static json_object *make_reply(void) {
    json_object *reply=json_object_new_object(),*data=json_object_new_object();
    assert(reply && data);
    char text[1024];memset(text,'x',sizeof(text)-1);text[sizeof(text)-1]=0;
    json_build_object_add(reply,"code",json_object_new_string("ok"));
    json_build_object_add(data,"status",json_object_new_string(text));
    json_build_object_add(reply,"data",data);
    watched[0]=reply;watched[1]=data;freed[0]=freed[1]=0;
    used=0;output[0]=0;return reply;
}
static void check_busy(json_object *reply) {
    send_json(7,reply);
    assert(freed[0]==1 && freed[1]==1);
    watched[0]=watched[1]=NULL;
    assert(strstr(output,"HTTP/1.1 503 ")==output);
    assert(strstr(output,"Content-Length: 15\r\n"));
    assert(!strcmp(strstr(output,"\r\n\r\n")+4,"{\"code\":\"busy\"}"));
}
int main(void) {
    json_object *reply=make_reply();
    json_build_object_add(json_object_object_get(reply,"data"),"required",NULL);
    check_busy(reply);
    reply=make_reply();fail_realloc=true;check_busy(reply);assert(!fail_realloc);
    send_json(7,make_reply());
    watched[0]=watched[1]=NULL;
    assert(strstr(output,"HTTP/1.1 200 ")==output);
    enum json_tokener_error error;
    reply=json_tokener_parse_verbose(strstr(output,"\r\n\r\n")+4,&error);
    assert(reply && error==json_tokener_success);
    assert(!strcmp(json_object_get_string(json_object_object_get(reply,"code")),"ok"));
    json_object_put(reply);
    fail_send=true;send_json(7,make_reply());
    assert(freed[0]==1 && freed[1]==1);watched[0]=watched[1]=NULL;
    puts("A1 HTTP releases owned JSON before socket send; busy/recovery/send failure passed");
}
'''


class HttpJsonLifetimeTest(unittest.TestCase):
    def test_owned_json_released_before_network_wait(self):
        source = (ROOT / 'A1/http.c').read_text()
        def fragment(start, end):
            return source[source.index(start):source.index(end, source.index(start))]
        functions = fragment('static bool response(', 'static bool read_request(')
        functions += fragment('static void send_json(', 'static json_object *read_json(')
        sdk = Path(os.environ.get('PHICOMM_TEST_JSON_DIR',
                                 ROOT / 'build/a1-sdk/mico-os/libraries/utilities/json_c'))
        with tempfile.TemporaryDirectory() as temporary:
            temporary = Path(temporary)
            probe = temporary / 'probe.c'
            probe.write_text(HARNESS.replace('FUNCTIONS', functions))
            binary = temporary / 'probe'
            command = [os.environ.get('CC', 'cc'), '-std=gnu99', '-Wall', '-Wextra',
                       '-Wno-unused-parameter', '-Wno-sign-compare', '-Wno-implicit-fallthrough',
                       '-Wno-unused-but-set-variable', '-Wno-unused-value', '-Wno-nonnull-compare',
                       '-I' + str(ROOT), '-I' + str(ROOT / 'tests/stubs'), '-I' + str(sdk)]
            if os.environ.get('SANITIZE') == '1':
                command += ['-g', '-fno-omit-frame-pointer', '-fsanitize=address,undefined']
            command += [str(probe), *map(str, sdk.glob('*.c')), '-Wl,--wrap=free',
                        '-Wl,--wrap=realloc', '-o', str(binary)]
            subprocess.run(command, check=True)
            subprocess.run([str(binary)], check=True)


def load_tests(loader, tests, pattern):
    # Keep the existing host-suite entry point covering the A1 transport checks.
    tests.addTests(loader.loadTestsFromName('tests.test_a1_http_owner'))
    return tests


if __name__ == '__main__':
    unittest.main()
