#!/usr/bin/env python3
"""Exercise the production HTTP origin check and pinned HAL snapshot helper."""
import argparse
import json
from pathlib import Path
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(ROOT))
from build import COPY_LOCAL_IP_FUNCTION

C_SOURCE = r'''
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "http_cors.h"
#include "http.h"

struct ipv4_config { unsigned address; };
struct wlan_ip_config { struct ipv4_config ipv4; };
typedef struct ip4_addr { uint32_t addr; } ip4_addr_t;
#define IP4ADDR_STRLEN_MAX 16

static int g_bOpenAccessPointMode;
static char g_IP[32] = "192.168.0.42";
static char netif_ip[32] = "192.168.0.42";
static int net_result;
static void *selected_interface;
static unsigned net_calls, hal_ip_calls;
static bool overwrite_legacy_buffer;
static pthread_mutex_t race_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t race_cv = PTHREAD_COND_INITIALIZER;
static bool overwrite_requested, overwrite_complete;
static char sta_handle, uap_handle;

const char *HAL_GetMyIPString(void)
{
    ++hal_ip_calls;
    return g_IP;
}

static void *overwrite_legacy_ip(void *unused)
{
    (void)unused;
    pthread_mutex_lock(&race_lock);
    while (!overwrite_requested) pthread_cond_wait(&race_cv, &race_lock);
    strcpy(g_IP, "192.168.0.99");
    overwrite_complete = true;
    pthread_cond_broadcast(&race_cv);
    pthread_mutex_unlock(&race_lock);
    return NULL;
}

static void inject_legacy_overwrite(void)
{
    pthread_t thread;
    overwrite_requested = overwrite_complete = false;
    assert(!pthread_create(&thread, NULL, overwrite_legacy_ip, NULL));
    pthread_mutex_lock(&race_lock);
    overwrite_requested = true;
    pthread_cond_broadcast(&race_cv);
    while (!overwrite_complete) pthread_cond_wait(&race_cv, &race_lock);
    pthread_mutex_unlock(&race_lock);
    assert(!pthread_join(thread, NULL));
}

void *net_get_sta_handle(void)
{
    return &sta_handle;
}

void *net_get_uap_handle(void)
{
    return &uap_handle;
}

int net_get_if_addr(struct wlan_ip_config *status, void *interface)
{
    struct in_addr address;
    ++net_calls;
    selected_interface = interface;
    if (interface != net_get_sta_handle() && interface != net_get_uap_handle()) return -1;
    if (overwrite_legacy_buffer) {
        overwrite_legacy_buffer = false;
        inject_legacy_overwrite();
    }
    if (net_result) return net_result;
    assert(inet_pton(AF_INET, netif_ip, &address) == 1);
    status->ipv4.address = address.s_addr;
    return 0;
}

char *ip4addr_ntoa_r(const ip4_addr_t *address, char *out, int capacity)
{
    struct in_addr ip = { .s_addr = address->addr };
    return inet_ntop(AF_INET, &ip, out, (socklen_t)capacity) ? out : NULL;
}

@SDK_COPY_FUNCTION@

extern int PH_BL_CopyLocalIPAddress(char *out, size_t capacity);
@LOCAL_ORIGIN_FUNCTION@

static bool request_origin(const char *origin, ph_http_request *request)
{
    char raw[1024];
    ph_http_parser parser;
    int length = origin
        ? snprintf(raw, sizeof(raw), "GET / HTTP/1.1\r\nOrigin: %s\r\n\r\n", origin)
        : snprintf(raw, sizeof(raw), "GET / HTTP/1.1\r\n\r\n");
    assert(length > 0 && (size_t)length < sizeof(raw));
    ph_http_parser_init(&parser);
    for (int i = 0; i < length; ++i) {
        int result = ph_http_parser_feed(&parser, (unsigned char)raw[i]);
        if (result < 0) return false;
        if (result == 1) {
            *request = parser.request;
            return true;
        }
    }
    return false;
}

static void set_ip(const char *ip)
{
    snprintf(netif_ip, sizeof(netif_ip), "%s", ip);
}

static void test_snapshot_api(void)
{
    char out[16] = "stale";
    set_ip("192.168.0.42");
    g_bOpenAccessPointMode = 0;
    assert(PH_BL_CopyLocalIPAddress(out, sizeof(out)));
    assert(!strcmp(out, "192.168.0.42") && selected_interface == net_get_sta_handle());
    assert(!PH_BL_CopyLocalIPAddress(out, 12) && out[0] == '\0');
    assert(!PH_BL_CopyLocalIPAddress(out, 0));
    assert(!PH_BL_CopyLocalIPAddress(NULL, sizeof(out)));

    g_bOpenAccessPointMode = 1;
    set_ip("10.23.4.1");
    assert(PH_BL_CopyLocalIPAddress(out, sizeof(out)));
    assert(!strcmp(out, "10.23.4.1") && selected_interface == net_get_uap_handle());

    net_result = -1;
    strcpy(out, "stale");
    assert(!PH_BL_CopyLocalIPAddress(out, sizeof(out)) && out[0] == '\0');
    net_result = 0;
    set_ip("0.0.0.0");
    assert(!PH_BL_CopyLocalIPAddress(out, sizeof(out)) && out[0] == '\0');
    set_ip("192.168.0.42");
}

static void test_origin_policy(void)
{
    ph_http_request request;
    g_bOpenAccessPointMode = 0;
    set_ip("192.168.0.42");

    assert(request_origin(NULL, &request) && local_origin(request.origin));
#if PH_HTTP_PORT == 80
    assert(request_origin("http://192.168.0.42", &request) && local_origin(request.origin));
#else
    assert(request_origin("http://192.168.0.42", &request) && !local_origin(request.origin));
#endif
    assert(request_origin("http://192.168.0.1", &request) && local_origin(request.origin));
    assert(request_origin("https://phicomm-io.github.io", &request) && local_origin(request.origin));
    assert(request_origin("https://evil.example", &request) && !local_origin(request.origin));
    assert(request_origin("null", &request) && !local_origin(request.origin));
    assert(request_origin("http://192.168.0.42.evil", &request) && !local_origin(request.origin));
    assert(request_origin("http://192.168.0.42/", &request) && !local_origin(request.origin));

#if PH_HTTP_PORT == 80
    assert(request_origin("http://192.168.0.42:80", &request) && local_origin(request.origin));
    assert(request_origin("http://192.168.0.42:81", &request) && !local_origin(request.origin));
#else
    assert(request_origin("http://192.168.0.42:8080", &request) && local_origin(request.origin));
    assert(request_origin("http://192.168.0.42", &request) && !local_origin(request.origin));
    assert(request_origin("http://192.168.0.42:80", &request) && !local_origin(request.origin));
    assert(request_origin("http://192.168.0.42:8080:80", &request) && !local_origin(request.origin));
#endif

    g_bOpenAccessPointMode = 1;
    set_ip("10.23.4.1");
#if PH_HTTP_PORT == 80
    assert(request_origin("http://10.23.4.1", &request) && local_origin(request.origin));
#else
    assert(request_origin("http://10.23.4.1:8080", &request) && local_origin(request.origin));
#endif
    assert(selected_interface == net_get_uap_handle());

    g_bOpenAccessPointMode = 0;
    set_ip("10.23.4.5");
#if PH_HTTP_PORT == 80
    assert(request_origin("http://10.23.4.5", &request) && local_origin(request.origin));
#else
    assert(request_origin("http://10.23.4.5:8080", &request) && local_origin(request.origin));
#endif
    assert(request_origin("http://192.168.0.42", &request) && !local_origin(request.origin));
}

static void test_origin_header_boundary(void)
{
    char origin[161];
    ph_http_request request;
    memset(origin, 'a', sizeof(origin) - 1);
    origin[159] = '\0';
    assert(request_origin(origin, &request));
    assert(strlen(request.origin) == 159 && !local_origin(request.origin));
    origin[159] = 'a';
    origin[160] = '\0';
    assert(!request_origin(origin, &request));
}

static void test_concurrent_legacy_buffer_overwrite(void)
{
    ph_http_request request;
    set_ip("192.168.0.42");
    strcpy(g_IP, "192.168.0.42");
    g_bOpenAccessPointMode = 0;
    overwrite_legacy_buffer = true;
#if PH_HTTP_PORT == 80
    assert(request_origin("http://192.168.0.42", &request));
#else
    assert(request_origin("http://192.168.0.42:8080", &request));
#endif
    assert(local_origin(request.origin));
    assert(!strcmp(g_IP, "192.168.0.99"));
    assert(!hal_ip_calls);
}

int main(void)
{
    test_snapshot_api();
    test_origin_policy();
    test_origin_header_boundary();
    test_concurrent_legacy_buffer_overwrite();
    printf("PASS: origin policy, HTTP header bound, %u caller-owned netif snapshots, concurrent legacy g_IP overwrite (listener port %u)\n",
           net_calls, (unsigned)PH_HTTP_PORT);
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run-dir', type=Path, required=True)
    args = parser.parse_args()
    run = args.run_dir.resolve()
    run.mkdir(parents=True, exist_ok=True)
    http_source = (ROOT / 'src' / 'ph_http.c').read_text()
    function_start = http_source.index('static bool local_origin(')
    function_open = http_source.index('{', function_start)
    depth = 0
    function_end = None
    for index in range(function_open, len(http_source)):
        if http_source[index] == '{':
            depth += 1
        elif http_source[index] == '}':
            depth -= 1
            if depth == 0:
                function_end = index + 1
                break
    if function_end is None:
        raise SystemExit('Could not extract local_origin from production source')
    local_origin = http_source[function_start:function_end]
    source = run / 'http-origin-test.c'
    generated = C_SOURCE.replace('@SDK_COPY_FUNCTION@', COPY_LOCAL_IP_FUNCTION)
    source.write_text(generated.replace('@LOCAL_ORIGIN_FUNCTION@', local_origin))
    compiler = 'clang'
    results = []
    for port in (80, 8080):
        executable = run / f'http-origin-{port}'
        log_path = run / f'http-origin-{port}-compiler.log'
        command = [
            compiler, '-std=c99', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
            '-Wno-misleading-indentation', '-Wno-sign-compare',
            '-Wno-unused-parameter', '-Wno-unused-function', '-Wno-unused-variable',
            '-Wno-unused-but-set-variable', '-Wno-missing-field-initializers',
            '-D_DEFAULT_SOURCE', '-DPH_HOST_TEST', f'-DPH_HTTP_PORT={port}',
            '-ffunction-sections', '-fdata-sections', '-fsanitize=address,undefined',
            '-fno-omit-frame-pointer', '-pthread',
            '-I', str(HERE), '-I', str(ROOT / 'src'), '-I', str(ROOT.parent / 'common'),
            '-I', str(ROOT / 'protocol' / 'httpd'),
            str(source), str(ROOT / 'protocol' / 'httpd' / 'parser.c'),
            '-Wl,--gc-sections', '-o', str(executable),
        ]
        with log_path.open('w') as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
        result = subprocess.run([str(executable)], text=True, capture_output=True, timeout=10)
        (run / f'http-origin-{port}-test.log').write_text(result.stdout + result.stderr)
        result.check_returncode()
        results.append(result.stdout.strip())
    (run / 'http-origin-test.log').write_text('\n'.join(results) + '\n')
    (run / 'http-origin-result.json').write_text(json.dumps({
        'status': 'PASS',
        'production_local_origin_compiled': True,
        'pinned_hal_overlay_function_used_verbatim': True,
        'http_parser_used_verbatim': True,
        'address_source': 'net_get_if_addr into local wlan_ip_config',
        'formatter': 'ip4addr_ntoa_r into caller buffer',
        'ports': [80, 8080],
        'asan_ubsan': True,
        'concurrent_legacy_buffer_overwrite': True,
        'cases': [
            'same-origin IPv4', 'trusted configured origin', 'cross-origin rejection',
            'literal null origin rejection', 'empty origin', 'AP IPv4', 'port boundaries',
            'malformed suffix/path rejection', 'Origin length 159 accepted and 160 rejected',
            'caller-owned netif API errors, zero address, and capacity failures',
            'reentrant caller-buffer IPv4 formatting',
        ],
        'hardware_verified': False,
    }, indent=2) + '\n')
    print('\n'.join(results))


if __name__ == '__main__':
    main()
