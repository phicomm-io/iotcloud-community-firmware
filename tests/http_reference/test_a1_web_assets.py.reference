"""Execute production A1/M1 static routes/parser; verify content-based validators."""
import hashlib
import importlib.util
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]


def fragment(source, start, end):
    begin = source.index(start)
    return source[begin:source.index(end, begin)]


class A1WebAssetsTest(unittest.TestCase):
    PRODUCT = "A1"

    def test_authenticated_assets_revalidate_without_redownloading(self):
        source = (ROOT / self.PRODUCT / 'http.c').read_text()
        parser = fragment(source, 'static bool read_request(', 'static bool authorized(')
        auth = fragment(source, 'static bool authorized(', 'static void str(')
        asset = fragment(source, 'static bool asset_response(', 'static void memory_response(')
        response = fragment(source, 'static bool response(', 'static void message(')
        # Execute static route branches, adapting only unrelated API dispatch.
        route = fragment(source, 'static void handle(', '    if (!strncmp(req.path, "/api/v1/", 8))')
        route += fragment(source, '    if (!strcmp(req.method,"GET") && (!strcmp(req.path,"/")', '    if(strcmp(req.method,"POST"))')
        route += '    message(fd,404,"Not found");\n}\n'
        request = fragment(source, 'typedef struct {', 'static volatile uint32_t http_accept_ok;')
        line = fragment(source, f'int {self.PRODUCT.lower()}_socket_line(', 'static bool response_send_until(')
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include "A1/favicon.h"
#include "common/http_cors.h"
static const char *response_cors;
#define A1_APP_OFFSET 0
#define A1_APP_LIMIT 262144
#define OTA_ENVELOPE_SIZE 128
#define A1_WEB_ETAG "\"new-page-content-hash\""
static const unsigned char a1_web_data[]={31,139,1,2,3,4};
typedef struct { char web_password[33]; } a1_config_t;
static char output[4096];static size_t used;
static const char *input;static size_t read_at;
static bool password;
static uint32_t mico_rtos_get_time(void) { return 100; }
static int a1_socket_read(int fd,void *out,size_t n,unsigned timeout) {
    assert(fd==7 && timeout>0);size_t left=strlen(input)-read_at;
    if(n>left)n=left;
    memcpy(out,input+read_at,n);read_at+=n;return n;
}
static bool response_send(int fd,const void *h,size_t hn,const void *b,size_t bn) {
    assert(fd==7 && hn+bn<sizeof(output));used=hn+bn;
    memcpy(output,h,hn);if(bn)memcpy(output+hn,b,bn);output[used]=0;return true;
}
RESPONSE
static void message(int fd,unsigned code,const char *text) {
    response(fd,code,"application/json",text,strlen(text),false);
}
static void a1_config_get(a1_config_t *c) { strcpy(c->web_password,password?"secret":""); }
REQUEST
LINE
PARSER
AUTH
ASSET
ROUTE
static void get(const char *raw) {
    input=raw;read_at=used=0;output[0]=0;handle(7);
}
static void unchanged(const char *raw) {
    get(raw);assert(!strncmp(output,"HTTP/1.1 304 ",13));
    assert(!strstr(output,"Content-Length:"));assert(strstr(output,"ETag: " A1_WEB_ETAG));
    assert((size_t)(strstr(output,"\r\n\r\n")+4-output)==used);
}
int main(void) {
    get("GET /favicon.ico HTTP/1.1\r\nHost: a1\r\n\r\n");
    assert(!strncmp(output,"HTTP/1.1 200 ",13));assert(strstr(output,"Content-Type: image/x-icon\r\n"));
    const char *body=strstr(output,"\r\n\r\n")+4;
    assert(used-(size_t)(body-output)==sizeof(a1_favicon_data));
    assert(!memcmp(body,a1_favicon_data,sizeof(a1_favicon_data)));
    get("GET / HTTP/1.1\r\nHost: a1\r\n\r\n");
    assert(strstr(output,"Content-Encoding: gzip\r\n"));
    assert(strstr(output,"Cache-Control: private, no-cache\r\n"));
    body=strstr(output,"\r\n\r\n")+4;
    assert(used-(size_t)(body-output)==sizeof(a1_web_data) && !memcmp(body,a1_web_data,sizeof(a1_web_data)));
    unchanged("GET / HTTP/1.1\r\nIf-None-Match: " A1_WEB_ETAG "\r\n\r\n");
    unchanged("GET /index.html HTTP/1.1\r\nif-none-match: W/" A1_WEB_ETAG "\r\n\r\n");
    get("GET / HTTP/1.1\r\nIf-None-Match: \"old-content\"\r\n\r\n");
    assert(!strncmp(output,"HTTP/1.1 200 ",13)); /* Changed bytes redownload. */
    get("GET /favicon.ico HTTP/1.1\r\nIf-None-Match: " A1_FAVICON_ETAG "\r\n\r\n");
    assert(!strncmp(output,"HTTP/1.1 304 ",13));
    password=true;
    get("OPTIONS /api/v1/info HTTP/1.1\r\nOrigin: https://phicomm-io.github.io\r\n\r\n");
    assert(!strncmp(output,"HTTP/1.1 200 ",13));
    assert(strstr(output,"Access-Control-Allow-Origin: https://phicomm-io.github.io\r\n"));
    assert(strstr(output,"Access-Control-Allow-Headers: Authorization, Content-Type\r\n"));
    get("GET /api/v1/info HTTP/1.1\r\nOrigin: https://mqtt.fjrcn.cn\r\n\r\n");
    assert(!strncmp(output,"HTTP/1.1 401 ",13));
    assert(strstr(output,"Access-Control-Allow-Origin: https://mqtt.fjrcn.cn\r\n"));
    get("OPTIONS /api/v1/info HTTP/1.1\r\nOrigin: https://phicomm-io.github.io.evil.example\r\n\r\n");
    assert(!strncmp(output,"HTTP/1.1 403 ",13) && !strstr(output,"Access-Control-Allow-Origin:"));

    get("GET / HTTP/1.1\r\nIf-None-Match: " A1_WEB_ETAG "\r\n\r\n");
    assert(!strncmp(output,"HTTP/1.1 401 ",13) && strstr(output,"Cache-Control: no-store"));
    get("GET /favicon.ico HTTP/1.1\r\n\r\n");assert(!strncmp(output,"HTTP/1.1 401 ",13));
    unchanged("GET / HTTP/1.1\r\nAuthorization: Basic YWRtaW46c2VjcmV0\r\nIf-None-Match: " A1_WEB_ETAG "\r\n\r\n");
    password=false;
    get("POST /favicon.ico HTTP/1.1\r\n\r\n");assert(!strncmp(output,"HTTP/1.1 404 ",13));
    get("GET /missing.ico HTTP/1.1\r\n\r\n");assert(!strncmp(output,"HTTP/1.1 404 ",13));
    response(7,200,"application/json","{}",2,false);
    assert(strstr(output,"Cache-Control: no-store\r\n") && !strstr(output,"ETag:"));
    puts("A1 assets: icon bytes, gzip, 304, changed tag, authentication and JSON no-store: OK");
}
'''
        harness = harness.replace("A1", self.PRODUCT).replace("a1", self.PRODUCT.lower())
        for token, text in [('RESPONSE', response), ('REQUEST', request), ('LINE', line), ('PARSER', parser),
                            ('AUTH', auth), ('ASSET', asset), ('ROUTE', route)]:
            harness = harness.replace('\n' + token + '\n', '\n' + text + '\n')
        with tempfile.TemporaryDirectory() as temporary:
            probe, binary = Path(temporary) / 'probe.c', Path(temporary) / 'probe'
            probe.write_text(harness)
            command = ['cc', '-std=c99', '-Wall', '-Wextra', '-Werror', '-I'+str(ROOT), str(probe), '-o', str(binary)]
            if os.environ.get('SANITIZE') == '1':
                command += ['-g', '-fno-omit-frame-pointer', '-fsanitize=address,undefined']
            subprocess.run(command, check=True)
            subprocess.run([str(binary)], check=True)

    def test_embedded_page_tag_tracks_content(self):
        sys.path.insert(0, str(ROOT / 'tools'))
        spec = importlib.util.spec_from_file_location('pack_frontend_test', ROOT / 'tools/pack_frontend.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            bundle = root / 'verified'
            page = bundle / 'devices' / self.PRODUCT.lower() / 'index.html.gz'
            page.parent.mkdir(parents=True)
            with patch.object(module, 'ROOT', root), patch.object(module, 'fetch', return_value=bundle):
                (root / 'common/web').mkdir(parents=True)
                (root / 'common/web/http_queue.js').write_bytes((ROOT / 'common/web/http_queue.js').read_bytes())
                previous = None
                import gzip
                for text in (b'original', b'updated'):
                    data=gzip.compress(b'<html><head></head><body>'+text+b'</body></html>',mtime=0)
                    page.write_bytes(data)
                    module.pack(self.PRODUCT.lower())
                    header = (root / self.PRODUCT / 'generated/web_data.h').read_text()
                    adapted=module.adapt_http_queue(data,self.PRODUCT.lower())
                    self.assertIn(b"const nativeFetch=window.fetch.bind(window)",gzip.decompress(adapted))
                    self.assertEqual(adapted,module.adapt_http_queue(data,self.PRODUCT.lower()))
                    digest = hashlib.sha256(adapted).hexdigest()
                    self.assertIn(f'#define {self.PRODUCT}_WEB_ETAG "\\\"' + digest + '\\\""', header)
                    self.assertNotEqual(previous, digest)
                    previous = digest
        # ICO directory and tag must match the actual image bytes.
        header = (ROOT / self.PRODUCT / 'favicon.h').read_text()
        data = bytes(int(x,16) for x in re.findall(r'0x([0-9a-f]{2})', header))
        self.assertIn(hashlib.sha256(data).hexdigest(), header)
        reserved, kind, count = struct.unpack_from('<HHH', data)
        self.assertEqual((reserved,kind,count), (0,1,2))
        for i in range(count):
            size, offset = struct.unpack_from('<II', data, 6+i*16+8)
            self.assertLessEqual(offset+size, len(data))
            self.assertEqual(data[offset:offset+8], b'\x89PNG\r\n\x1a\n')


class M1WebAssetsTest(A1WebAssetsTest):
    PRODUCT = "M1"


class TC1WebQueueAssetsTest(unittest.TestCase):
    def test_both_variants_embed_the_queue_in_the_c_array(self):
        import gzip
        sys.path.insert(0, str(ROOT / 'tools'))
        import pack_frontend as module
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            bundle = root / 'verified'
            pages = bundle / 'devices/tc1'
            pages.mkdir(parents=True)
            (root / 'common/web').mkdir(parents=True)
            adapter = (ROOT / 'common/web/http_queue.js').read_bytes()
            (root / 'common/web/http_queue.js').write_bytes(adapter)
            with patch.object(module, 'ROOT', root), patch.object(module, 'fetch', return_value=bundle):
                for variant, name in [('full', 'index.html.gz'), ('no-frontend-resources', 'setup.html.gz')]:
                    raw = gzip.compress(b'<html><head></head><body>' + name.encode() + b'</body></html>', mtime=0)
                    (pages / name).write_bytes(raw)
                    with patch.dict(os.environ, {'PHICOMM_FIRMWARE_VARIANT': variant}):
                        module.pack('tc1')
                    source = (root / 'TC1/generated/web_data.c').read_text()
                    embedded = bytes(int(x, 16) for x in re.findall(r'0x([0-9a-f]{2})', source))
                    self.assertEqual(gzip.decompress(embedded).count(adapter), 1)
                    self.assertEqual(embedded, module.adapt_http_queue(raw, 'tc1'))
