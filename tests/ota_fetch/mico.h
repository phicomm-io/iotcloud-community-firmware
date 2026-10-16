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
static inline int MicoRandomNumberRead(void *p,unsigned n){return getrandom(p,n,0)==n?0:-1;}
static inline int micoWlanGetIPStatus(IPStatusTypedef *ip,int station){(void)station;snprintf(ip->dns,16,"%s",getenv("OTA_TEST_DNS")?getenv("OTA_TEST_DNS"):"127.0.0.1");return 0;}
