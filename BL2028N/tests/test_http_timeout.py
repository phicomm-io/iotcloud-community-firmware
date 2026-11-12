#!/usr/bin/env python3
"""Run the overlaid OpenBeken TCP reader against real local sockets."""
import argparse
import json
from pathlib import Path
import subprocess

STUBS = r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#define ADDLOG_ERROR(...) ((void)0)
static uint32_t rtos_get_time(void) {
    struct timespec t;assert(!clock_gettime(CLOCK_MONOTONIC,&t));
    return (uint32_t)((uint64_t)t.tv_sec*1000+t.tv_nsec/1000000);
}
'''

SCENARIOS = r'''
static void nap(unsigned ms) {
    struct timespec t={ms/1000,(ms%1000)*1000000};
    while(nanosleep(&t,&t)<0)assert(errno==EINTR);
}
static void *writer(void *arg) {nap(40);assert(send(*(int*)arg,"xyz",3,0)==3);return NULL;}
static volatile sig_atomic_t interrupted;
static void alarm_handler(int sig) {(void)sig;interrupted=1;}
int main(void) {
    int fds[2];char buf[16];uint64_t start,elapsed;
    assert(utils_time_left(1000,950)==50);
    assert(utils_time_left(1000,1000)==0 && utils_time_left(1000,1001)==0);
    assert(utils_time_left(25,UINT32_MAX-24)==50);
    assert(utils_time_left(UINT32_MAX-24,25)==0);
    assert(!socketpair(AF_UNIX,SOCK_STREAM,0,fds));
    start=utils_time_get_ms();assert(HAL_TCP_Read(fds[0],buf,sizeof buf,80)==0);
    elapsed=utils_time_get_ms()-start;assert(elapsed>=60 && elapsed<1000);
    assert(HAL_TCP_Read(fds[0],buf,sizeof buf,0)==0);
    assert(send(fds[1],"ab",2,0)==2);
    assert(HAL_TCP_Read(fds[0],buf,sizeof buf,200)==2 && !memcmp(buf,"ab",2));
    assert(send(fds[1],"0123456789abcdef",16,0)==16);
    assert(HAL_TCP_Read(fds[0],buf,sizeof buf,200)==16 && !memcmp(buf,"0123456789abcdef",16));
    struct sigaction action={0};action.sa_handler=alarm_handler;
    assert(!sigemptyset(&action.sa_mask));assert(!sigaction(SIGALRM,&action,NULL));
    struct itimerval timer={0};timer.it_value.tv_usec=10000;assert(!setitimer(ITIMER_REAL,&timer,NULL));
    pthread_t thread;assert(!pthread_create(&thread,NULL,writer,&fds[1]));
    assert(HAL_TCP_Read(fds[0],buf,sizeof buf,300)==3 && !memcmp(buf,"xyz",3));
    assert(interrupted && !pthread_join(thread,NULL));
    close(fds[1]);assert(HAL_TCP_Read(fds[0],buf,sizeof buf,200)==-1);
    close(fds[0]);assert(HAL_TCP_Read(fds[0],buf,sizeof buf,200)==-2);
    puts("PASS: native TCP reader bounds silence, expired deadlines, partial/full data, EINTR, EOF and select failure");
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app-source', type=Path, required=True)
    parser.add_argument('--run-dir', type=Path, required=True)
    args = parser.parse_args()
    text = (args.app_source/'src/httpclient/utils_net.c').read_text()
    start = text.index('int32_t HAL_TCP_Read(')
    end = text.index('\n/*** TCP connection ***/', start)
    run = args.run_dir.resolve()
    run.mkdir(parents=True, exist_ok=True)
    source = run/'native-http-read.c'
    timer = (args.app_source/'src/httpclient/utils_timer.c').read_text()
    timer = timer[timer.index('uint32_t utils_time_get_ms(void)'):timer.rindex('#endif')]
    source.write_text(STUBS+timer+text[start:end]+SCENARIOS)
    executable = run/'native-http-read'
    subprocess.run(['clang', '-g', '-O1', '-Wall', '-Wextra', '-Werror', '-Wno-sign-compare',
                    '-fsanitize=address,undefined', '-pthread', str(source),
                    '-o', str(executable)], check=True)
    result = subprocess.run([str(executable)], text=True, capture_output=True, timeout=8)
    (run/'test.log').write_text(result.stdout+result.stderr)
    result.check_returncode()
    (run/'result.json').write_text(json.dumps({
        'status': 'PASS', 'native_tcp_reader_verbatim': True,
        'native_timer_helpers_verbatim': True,
        'real_local_sockets': True, 'asan_ubsan': True,
        'cases': ['silent peer timeout', 'expired deadline', 'partial body',
                  'full buffer', 'EINTR and delayed data', 'EOF', 'select error',
                  'remaining milliseconds', '32-bit clock wrap'],
    }, indent=2)+'\n')
    print(result.stdout, end='')


if __name__ == '__main__':
    main()
