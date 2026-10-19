"""Execute the patched SDK's real client loop across recoverable socket errors."""
import os
from pathlib import Path
import unittest
from tests import test_memory_admission as admission
from tests.test_tc1_sdk_migration import function
ROOT = Path(__file__).resolve().parents[1]
class Recovery(unittest.TestCase):
 def test_reset_select_and_close_do_not_suspend_listener(self):
  helper=admission.MemoryAdmission();source=helper.sdk_http_source('httpd.c')
  prefix=r'''
#include <assert.h>
#include <stdbool.h>
#include <string.h>
#include <sys/select.h>
#define FALSE false
#define HTTPD_TIMEOUT_EVENT 0
#define HTTPD_CLIENT_SOCK_TIMEOUT 2
#define HTTPD_DONE 1
#define kNoErr 0
#define httpd_d(...) ((void)0)
static bool httpd_stop_req;
static int client_sockfd=-1,select_failure,close_failure,closes,handled,selects;
static void httpd_suspend_thread(bool fault){(void)fault;assert(!"HTTP service suspended");}
static int net_get_sock_error(int fd){(void)fd;return -1;}
static int fake_select(int n,fd_set *r,fd_set *w,fd_set *e,struct timeval *t){
 (void)n;(void)r;(void)w;(void)e;(void)t;selects++;return select_failure?-1:1;
}
#define select fake_select
SELECT
static int httpd_accept_client_socket(const fd_set *fds){assert(fds);client_sockfd=7;return 0;}
static int fake_close(int fd){assert(fd==7);closes++;return close_failure?-1:0;}
#define close fake_close
static int httpd_handle_message(int fd){assert(fd==7);handled++;return HTTPD_DONE;}
CLIENT
int main(void){
 fd_set fds;FD_ZERO(&fds);FD_SET(3,&fds);
 select_failure=close_failure=1;httpd_handle_client_connection(&fds);
 assert(client_sockfd==-1 && closes==1 && handled==0 && selects==1);
 select_failure=0;httpd_handle_client_connection(&fds);
 assert(client_sockfd==-1 && closes==2 && handled==1 && selects==2);
 close_failure=0;httpd_handle_client_connection(&fds);
 assert(client_sockfd==-1 && closes==3 && handled==2 && selects==3);
}
'''
  helper.run_c(prefix.replace('SELECT\n','\n'+function(source,'static int httpd_select(')).replace('\nCLIENT\n','\n'+function(source,'static void httpd_handle_client_connection(')))
if __name__=='__main__':unittest.main()
