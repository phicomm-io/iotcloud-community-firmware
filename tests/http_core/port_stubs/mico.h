#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <fcntl.h>
typedef uintptr_t mico_thread_arg_t;
#define kNoErr 0
#define kInProgressErr 1
#define MICO_APPLICATION_PRIORITY 1
#define TCP_MAX_CONN_NUM 0x93
#define TCP_CONN_NUM 0x94
int mico_rtos_create_thread(void *,int,const char *,void (*)(mico_thread_arg_t),unsigned,mico_thread_arg_t);
void mico_rtos_delete_thread(void *);
void mico_thread_msleep(unsigned);
uint32_t mico_rtos_get_time(void);
int ph_socket(int,int,int);int ph_bind(int,const struct sockaddr *,socklen_t);int ph_listen(int,int);
int ph_setsockopt(int,int,int,const void *,socklen_t);int ph_getsockopt(int,int,int,void *,socklen_t *);
int ph_fcntl(int,int,...);int ph_select(int,fd_set *,fd_set *,fd_set *,struct timeval *);
int ph_accept(int,struct sockaddr *,socklen_t *);ssize_t ph_recv(int,void *,size_t,int);ssize_t ph_send(int,const void *,size_t,int);int ph_close(int);
#define socket ph_socket
#define bind ph_bind
#define listen ph_listen
#define setsockopt ph_setsockopt
#define getsockopt ph_getsockopt
#define fcntl ph_fcntl
#define select ph_select
#define accept ph_accept
#define recv ph_recv
#define send ph_send
#define close ph_close
