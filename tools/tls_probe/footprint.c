#include "profile.h"
#include "signed_ota.h"
#include "download.h"
#ifndef PROBE_BUFFER_SIZE
#define PROBE_BUFFER_SIZE 2048
#endif
#ifndef OTA_ONLY
probe_context probe;
unsigned char record_buffer[PROBE_BUFFER_SIZE];
/* Not linked: object symbols expose target ABI sizes for the report. */
unsigned char size_client[sizeof(br_ssl_client_context)];
unsigned char size_decoder[sizeof(probe_x509)];
unsigned char size_io[sizeof(br_sslio_context)];
static int read_cb(void *c,unsigned char *p,size_t n){(void)c;(void)p;(void)n;return -1;}
static int write_cb(void *c,const unsigned char *p,size_t n){(void)c;(void)p;return (int)n;}
#endif
#ifndef TLS_ONLY
signed_ota session;
static bool erase_cb(void *c) {(void)c;return true;}
static bool flash_write(void *c,uint32_t o,const uint8_t *p,size_t n) {(void)c;(void)o;(void)p;(void)n;return true;}
static bool flash_read(void *c,uint32_t o,uint8_t *p,size_t n) {(void)c;(void)o;(void)p;(void)n;return false;}
#endif
#ifdef DOWNLOAD_CORE
probe_download_result result;
static uint32_t clock_cb(void *c) {(void)c;return 0;}
static void close_cb(void *c) {(void)c;}
static int open_cb(void *c, probe_context *p, void *buffer, size_t length, const char *host)
{
    unsigned char entropy[32]={0};
    (void)c;
    probe_init(p,buffer,length);
    br_ssl_engine_inject_entropy(&p->client.eng,entropy,sizeof(entropy));
    br_ssl_client_reset(&p->client,host,0);
    br_sslio_init(&p->io,&p->client.eng,read_cb,0,write_cb,0);
    return 1;
}
#endif
void _start(void) {
#ifdef DOWNLOAD_CORE
    probe_download_config config={
        "phicomm-io.github.io", "/releases/v0.9.5/phicomm_a1.ota.bin.signed",
        sizeof(record_buffer), 1024, 3, {erase_cb,flash_write,flash_read,0},
        0,open_cb,close_cb,clock_cb,0,0,0
    };
    probe_download(&config,&probe,record_buffer,&result);
#else
    unsigned char chunk[512]={0};
#ifndef OTA_ONLY
    probe_init(&probe,record_buffer,sizeof(record_buffer));
    br_ssl_engine_inject_entropy(&probe.client.eng,chunk,32);
    br_ssl_client_reset(&probe.client,"phicomm-io.github.io",0);
    br_sslio_init(&probe.io,&probe.client.eng,read_cb,0,write_cb,0);
    br_sslio_write_all(&probe.io,chunk,1);
    br_sslio_flush(&probe.io);
    br_sslio_read(&probe.io,chunk,sizeof(chunk));
#endif
#ifndef TLS_ONLY
    signed_ota_io io={erase_cb,flash_write,flash_read,0};
    signed_ota_begin(&session,647856,1048576,3,1,io);
    signed_ota_write(&session,chunk,sizeof(chunk));
    signed_ota_finish(&session);
#endif
#endif
    for(;;){}
}
