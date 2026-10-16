"""A failed MiCO connection must not close a recycled descriptor on cleanup."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from tests.test_tc1_sdk_migration import ROOT, function


class NetworkClose(unittest.TestCase):
    def test_failed_connect_then_disconnect_closes_once(self):
        sdk = Path(os.environ.get('TC1_SDK_DIR', ROOT / 'build/tc1-sdk-3.6.4/mico-os'))
        path = sdk / 'libraries/protocols/mqtt/mico/MQTTMiCO.c'
        if not path.exists():
            self.skipTest('build TC1 first')
        harness = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
typedef int OSStatus;
typedef struct { int my_socket; } Network;
#define kNoErr 0
#define kUnknownErr -1
#define MQTT_CLIENT_SOCKET_TIMEOUT 5000
#define MQTT_CLIENT_SOCKET_TCP_KEEPIDLE 10
#define MQTT_CLIENT_SOCKET_TCP_KEEPINTVL 10
#define MQTT_CLIENT_SOCKET_TCP_KEEPCNT 3
#define mqtt_mico_log(...) ((void)0)
static int mode, closes;
static int fake_socket(int a,int b,int c) {return 11;}
static int fake_close(int fd) {assert(fd==11);closes++;return 0;}
static int fake_setsockopt(int fd,int level,int opt,const void *value,unsigned size) {
 return (mode==1&&opt==SO_SNDTIMEO)||(mode==2&&opt==SO_RCVTIMEO)?-1:0;
}
static int fake_connect(int fd,const struct sockaddr *addr,unsigned size) {return mode==3?-1:0;}
static int usergethostbyname(const char *domain,uint8_t *addr,uint8_t size) {strcpy((char*)addr,"127.0.0.1");return 0;}
#define socket fake_socket
#define close fake_close
#define setsockopt fake_setsockopt
#define connect fake_connect
'''
        source = path.read_text(errors='replace')
        for signature in ('void MICO_disconnect(', 'int SSL_ConnectNetwork(', 'int ConnectNetwork('):
            harness += function(source, signature)
        harness += r'''
int main(void) {
 for(int ssl=0;ssl<2;ssl++)for(mode=0;mode<4;mode++) {
  Network n={-1};closes=0;
  int rc=ssl?SSL_ConnectNetwork(&n,"localhost",1883,0,0):ConnectNetwork(&n,"localhost",1883);
  if(mode)assert(rc<0 && n.my_socket==-1 && closes==1);
  else assert(rc==0 && n.my_socket==11 && closes==0);
  MICO_disconnect(&n);assert(n.my_socket==-1 && closes==1);
  MICO_disconnect(&n);assert(closes==1);
 }
}
'''
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            (directory / 'network.c').write_text(harness)
            subprocess.run(['cc', '-std=c99', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                            str(directory / 'network.c'), '-o', str(directory / 'network')], check=True)
            subprocess.run([str(directory / 'network')], check=True, timeout=10)


if __name__ == '__main__':
    unittest.main()
