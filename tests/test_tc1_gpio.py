"""Regression: six socket writes must not overwrite adjacent configuration."""
import pathlib,subprocess,tempfile,unittest
ROOT=pathlib.Path(__file__).resolve().parents[1]
class SocketConfig(unittest.TestCase):
    def test_configuration_bounds_and_invalid_values(self):
        source=(ROOT/'TC1/user_gpio.c').read_text()
        start=source.index('void SetSocketStatus(')
        function=source[start:source.index('\n/*UserRelaySet',start)]
        harness=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define SOCKET_NUM 6
static struct {char sockets_guard[4];char socket_status[6];char user[31];} config;
static void *sys_config;
static unsigned relays, publications, saves;
#define user_config (&config)
static void UserRelaySet(int i,char on){assert(i>=0 && i<6 && on>=0 && on<=1);++relays;}
static void UserMqttSendSocketState(int i){(void)i;++publications;}
static void UserMqttSendTotalSocketState(void){++publications;}
static void mico_system_context_update(void *p){(void)p;++saves;}
'''+function+r'''
int main(void){
 memset(&config,0xa5,sizeof(config));
 SetSocketStatus("0,1,0,1,0,1,");
 for(int i=0;i<6;++i)assert(config.socket_status[i]==i%2);
 for(unsigned i=0;i<sizeof(config.user);++i)assert((unsigned char)config.user[i]==0xa5);
 for(unsigned i=0;i<sizeof(config.sockets_guard);++i)assert((unsigned char)config.sockets_guard[i]==0xa5);
 assert(relays==6 && publications==7 && saves==1);
 char previous[sizeof(config)];memcpy(previous,&config,sizeof(config));
 SetSocketStatus("1,1,1");SetSocketStatus("1,1,1,1,1,2");
 assert(!memcmp(previous,&config,sizeof(config)) && relays==6 && saves==1);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            src=pathlib.Path(directory)/'test.c';binary=pathlib.Path(directory)/'test'
            src.write_text(harness)
            subprocess.run(['cc','-std=c99','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(src),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True,timeout=5)
if __name__=='__main__':unittest.main()
