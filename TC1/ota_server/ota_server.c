/* Bounded direct HTTP transport. Authenticity comes from the signed package. */
#include "mico.h"
#include "../../common/ota/fetch.h"
#include "../../common/ota/mode.h"
#include "ota_server.h"
#include "signed_adapter.h"
#include "user_ota.h"
#include "../../common/ota/signed_ota.h"
#include <ctype.h>
#include <string.h>
static ota_server_cb_fn callback;
int ota_socket_read(int fd, void *data, size_t n, unsigned timeout)
{
    fd_set readfds; FD_ZERO(&readfds); FD_SET(fd, &readfds);
    struct timeval tv = { timeout / 1000, (timeout % 1000) * 1000 };
    if (select(fd + 1, &readfds, NULL, NULL, &tv) <= 0) return -1;
    return recv(fd, data, n, 0);
}

bool ota_socket_send(int fd, const void *data, size_t n)
{
    const uint8_t *p = data;
    while (n) {
        fd_set writefds; FD_ZERO(&writefds); FD_SET(fd, &writefds);
        struct timeval tv = {10, 0};
        if (select(fd + 1, NULL, &writefds, NULL, &tv) <= 0) return false;
        int sent = send(fd, p, n, 0);
        if (sent <= 0) return false;
        p += sent; n -= sent;
    }
    return true;
}

int ota_socket_line(int fd, char *line, size_t capacity)
{
    uint32_t deadline = mico_rtos_get_time() + 10000;
    size_t used = 0;
    while (used + 1 < capacity) {
        uint32_t now = mico_rtos_get_time();
        if ((int32_t)(now - deadline) >= 0) return -1;
        char c;
        if (ota_socket_read(fd, &c, 1, deadline - now) != 1 || !c) return -1;
        if (c == '\n') {
            if (!used || line[used - 1] != '\r') return -1;
            line[--used] = 0; return used;
        }
        line[used++] = c;
    }
    return -1;
}

static bool fetch_begin(void *ctx,uint32_t n){(void)ctx;return tc1_ota_begin(n,false);}
static bool fetch_write(void *ctx,uint32_t offset,const uint8_t *p,size_t n){
    (void)ctx;(void)offset;bool ok=tc1_ota_write(p,n);char state[32],err[128];unsigned progress;
    tc1_ota_snapshot(state,err,&progress);ota_progress=progress;if(callback)callback(OTA_LOADING,progress);return ok;
}
static void download_worker(mico_thread_arg_t arg){
    char *url=(char *)arg;const char *error="Cannot enter OTA mode";
    bool ok=ota_job_enter() && ota_fetch(url,0xa9000u+OTA_ENVELOPE_SIZE,fetch_begin,fetch_write,NULL,&error);
    if(ok)ok=tc1_ota_finish();
    if(ok){ota_progress=100;if(callback)callback(OTA_SUCCE,100);free(url);mico_thread_msleep(1500);mico_system_power_perform(mico_system_context_get(),eState_Software_Reset);}
    else{custom_log("OTA","%s",error?error:"Image validation failed");ota_progress=-1;if(callback)callback(OTA_FAIL,0);tc1_ota_failure(error);free(url);if(!tc1_ota_commit_uncertain())ota_job_release();}
    mico_rtos_delete_thread(NULL);
}

OSStatus ota_server_start(char *url,char *md5,ota_server_cb_fn cb)
{
    (void)md5; /* A broker-supplied MD5 is never an authorization credential. */
    if(!ota_url_valid(url) || !ota_job_claim())return kParamErr;
    if(!tc1_ota_reserve()){ota_job_release();return kParamErr;}
    char *copy=malloc(strlen(url)+1);
    if(!copy){tc1_ota_abort();ota_job_release();return kNoMemoryErr;}strcpy(copy,url);callback=cb;ota_progress=0;
    OSStatus err=mico_rtos_create_thread(NULL,MICO_APPLICATION_PRIORITY+1,"OTA",download_worker,6144,(mico_thread_arg_t)copy);
    if(err!=kNoErr){free(copy);tc1_ota_abort();ota_job_release();}return err;
}
