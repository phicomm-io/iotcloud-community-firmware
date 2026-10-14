"""Exercise the TC1-patched SDK sender after running TC1/build.sh."""
import pathlib, subprocess, tempfile, unittest
ROOT = pathlib.Path(__file__).resolve().parents[1]
class HttpSender(unittest.TestCase):
    def test_partial_send_and_bounded_failures(self):
        path = ROOT / 'build/tc1-sdk/mico-os/libraries/daemons/http_server/httpd_handle.c'
        if not path.exists(): self.skipTest('build the TC1 SDK first')
        source = path.read_text()
        function = source[source.index('int httpd_send(int conn,'):source.index('int httpd_recv(', source.index('int httpd_send(int conn,'))]
        harness = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <sys/select.h>
#define kInProgressErr 1
#define kNoErr 0
#define httpd_d(...) ((void)0)
static unsigned ticks, calls, written;
static int mode;
static uint32_t mico_rtos_get_time(void) { ticks+=200;return ticks; }
static int fake_select(int n,void*r,fd_set*w,void*e,struct timeval*t) {
 (void)n;(void)r;(void)w;(void)e;assert(t->tv_sec==0 && t->tv_usec==200000);
 ++calls;return mode==1?0:mode==2?-1:1;
}
static int fake_send(int fd,const char*b,int length,int flags) {
 (void)fd;(void)flags;
 if(mode==3)return 0;
 if(mode==4)return -1;
 int n=length>2?2:length;assert(!memcmp(b,"abcde"+written,n));written+=n;return n;
}
#define select fake_select
#define send fake_send
'''
        harness += function + r'''
int main(void) {
 assert(httpd_send(3,"abcde",5)==0);assert(written==5 && calls==3);
 for(mode=1;mode<=4;++mode){ticks=calls=0;assert(httpd_send(3,"abcde",5)<0);assert(calls<=25);}
}
'''
        with tempfile.TemporaryDirectory() as directory:
            src=pathlib.Path(directory)/'test.c';binary=pathlib.Path(directory)/'test'
            src.write_text(harness)
            subprocess.run(['cc','-std=c99','-Wall','-Wextra','-Werror',str(src),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True,timeout=5)
    def test_receive_timeout_peer_close_and_data(self):
        path = ROOT / 'build/tc1-sdk/mico-os/libraries/daemons/http_server/httpd_handle.c'
        if not path.exists(): self.skipTest('build the TC1 SDK first')
        source = path.read_text()
        function = source[source.index('int httpd_recv('):source.index('int httpd_send_hdr_from_code(')]
        harness = r'''
#include <assert.h>
#include <stddef.h>
#include <sys/select.h>
static int mode, reads;
static int fake_select(int n, fd_set*r, void*w, void*e, struct timeval*t) {
 (void)n;(void)r;(void)w;(void)e;assert(t->tv_sec==5 && t->tv_usec==0);
 return mode==0?0:mode==1?-1:1;
}
static int fake_recv(int fd,void*b,size_t n,int flags) {
 (void)fd;(void)n;(void)flags;++reads;
 if(mode==2)return 0;
 if(mode==3)return -1;
 *(char*)b='x';return 1;
}
#define select fake_select
#define recv fake_recv
'''
        harness += function + r'''
int main(void) {
 char b=0;
 for(mode=0;mode<4;mode++)assert(httpd_recv(3,&b,1,0)==(mode==2?0:-1));
 assert(reads==2);mode=4;assert(httpd_recv(3,&b,1,0)==1 && b=='x');
}
'''
        with tempfile.TemporaryDirectory() as directory:
            src=pathlib.Path(directory)/'test.c';binary=pathlib.Path(directory)/'test'
            src.write_text(harness)
            subprocess.run(['cc','-std=c99','-Wall','-Wextra','-Werror',str(src),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True,timeout=5)
if __name__ == '__main__': unittest.main()
