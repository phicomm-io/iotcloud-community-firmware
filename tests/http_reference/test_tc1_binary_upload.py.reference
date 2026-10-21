"""Exercise the SDK binary upload reader with a full buffer and read failures."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from tests.test_tc1_sdk_migration import ROOT, function


class BinaryUpload(unittest.TestCase):
    def test_fragmented_body_bounds_and_header_failures(self):
        sdk = Path(os.environ.get('TC1_SDK_DIR', ROOT / 'build/tc1-sdk-3.6.4/mico-os'))
        path = sdk / 'libraries/daemons/http_server/httpd_wsgi.c'
        if not path.exists():
            self.skipTest('build TC1 first')
        source = function(path.read_text(), 'int httpd_get_data2(')
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#define kNoErr 0
#define kInProgressErr 1
#define HTTPD_MAX_MESSAGE 256
typedef struct { int sock, hdr_parsed, chunked, remaining_bytes; } httpd_request_t;
static int parse_error, read_result=3, reads, parse_calls;
static int httpd_parse_hdr_tags(httpd_request_t *r,int fd,char *p,int n) {
 (void)fd;(void)p;(void)n;parse_calls++;r->remaining_bytes=8;return parse_error;
}
static int httpd_recv(int fd,char *p,int n,int flags) {
 (void)fd;(void)flags;reads++;if(read_result<=0)return read_result;
 int got=read_result<n?read_result:n;memset(p,0xa5,got);return got;
}
'''
        harness += source + r'''
int main(void) {
 struct { char body[4]; unsigned char guard; } buffer={{0},0x7e};
 httpd_request_t req={0};read_result=4;
 assert(httpd_get_data2(&req,buffer.body,4)==4 && req.remaining_bytes==4);
 assert(buffer.guard==0x7e && parse_calls==1);
 read_result=3;assert(httpd_get_data2(&req,buffer.body,4)==3 && req.remaining_bytes==1);
 assert(httpd_get_data2(&req,buffer.body,4)==1 && !req.remaining_bytes);
 int before=reads;assert(httpd_get_data2(&req,buffer.body,4)==0 && reads==before);
 req.remaining_bytes=4;read_result=0;assert(httpd_get_data2(&req,buffer.body,4)<0 && req.remaining_bytes==4);
 read_result=-1;assert(httpd_get_data2(&req,buffer.body,4)<0 && req.remaining_bytes==4);
 req.hdr_parsed=0;parse_error=-1;before=reads;assert(httpd_get_data2(&req,buffer.body,4)<0 && reads==before);
 req.hdr_parsed=1;req.chunked=1;assert(httpd_get_data2(&req,buffer.body,4)<0 && reads==before);
 req.chunked=0;req.remaining_bytes=-1;assert(httpd_get_data2(&req,buffer.body,4)<0 && reads==before);
 assert(httpd_get_data2(&req,buffer.body,0)<0);
}
'''
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            (directory / 'reader.c').write_text(harness)
            subprocess.run(['cc', '-std=c99', '-Wall', '-Wextra', '-Werror',
                            str(directory / 'reader.c'), '-o', str(directory / 'reader')], check=True)
            subprocess.run([str(directory / 'reader')], check=True, timeout=10)


if __name__ == '__main__':
    unittest.main()
