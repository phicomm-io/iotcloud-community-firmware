#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <string.h>
#include <sys/random.h>
#define kNoErr 0
#define Station 0
typedef struct {char dns[16];} IPStatusTypedef;
static inline uint32_t mico_rtos_get_time(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint32_t)(t.tv_sec*1000+t.tv_nsec/1000000);}
static inline void mico_thread_msleep(unsigned ms){usleep(ms*1000);}
static inline int MicoRandomNumberRead(void *p,unsigned n){return getrandom(p,n,0)==n?0:-1;}
static inline int micoWlanGetIPStatus(IPStatusTypedef *ip,int station){(void)station;snprintf(ip->dns,16,"%s",getenv("OTA_TEST_DNS")?getenv("OTA_TEST_DNS"):"127.0.0.1");return 0;}
#include <pthread.h>
#include <semaphore.h>
#include <errno.h>
typedef void *mico_ssl_t;
typedef sem_t *mico_semaphore_t;
typedef uintptr_t mico_thread_arg_t;
#define MICO_APPLICATION_PRIORITY 7
#define MICO_WAIT_FOREVER UINT32_MAX
#define TLS_V1_2_MODE 4
int mico_rtos_init_semaphore(mico_semaphore_t *,int);
int mico_rtos_set_semaphore(mico_semaphore_t *);
int mico_rtos_get_semaphore(mico_semaphore_t *,uint32_t);
int mico_rtos_deinit_semaphore(mico_semaphore_t *);
int mico_rtos_create_thread(void *,unsigned,const char *,void (*)(mico_thread_arg_t),unsigned,mico_thread_arg_t);
void mico_rtos_delete_thread(void *);
void ssl_set_client_version(unsigned);
mico_ssl_t ssl_connect_sni(int,int,char *,char *,int *);
void ssl_set_using_nonblock(mico_ssl_t,int);
int ssl_send(mico_ssl_t,void *,size_t);
int ssl_recv(mico_ssl_t,void *,size_t);
int ssl_pending(mico_ssl_t);
int ssl_get_error(mico_ssl_t,int);
int ssl_close(mico_ssl_t);
int ota_test_connect(int,const struct sockaddr *,socklen_t);
#define connect ota_test_connect
