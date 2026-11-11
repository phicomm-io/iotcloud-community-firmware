/* Native HTTP client boundary; callback tests supply response events. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define HTTPCLIENT_GET 0
#define HTTPREQUEST_FLAG_FREE_SELFONDONE 1
typedef struct {int response_code;} httpclient_t;
typedef struct {bool is_chunked;uint32_t response_content_len,response_buf_filled,response_buf_len;char *response_buf;} httpclient_data_t;
typedef struct httprequest_t_tag {
 int state,port,method,flags;
 uint32_t timeout;
 const char *url,*header;
 httpclient_t client;
 httpclient_data_t client_data;
 int (*data_callback)(struct httprequest_t_tag *);
} httprequest_t;
void *os_malloc(size_t);
void os_free(void *);
int HTTPClient_Async_SendGeneric(httprequest_t *);
