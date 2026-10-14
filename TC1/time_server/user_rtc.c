#include "http_server/web_log.h"
#include "main.h"
#include "user_gpio.h"
#include "sntp.h"
#include "mqtt_server/user_mqtt_client.h"

void RtcThread(mico_thread_arg_t arg);

OSStatus UserSntpGetTime()
{
    OSStatus err = kNoErr;
    ntp_timestamp_t current_time;

    struct hostent * hostent_content = NULL;
    char ** pptr = NULL;
    struct in_addr ipp;

    ipp.s_addr = 0xd248912c;
    err = sntp_get_time(&ipp, &current_time);

    if (err != kNoErr)
    {
        int ntp_count = 5;
        char* ntp_hosts[5] = {
            "pool.ntp.org",
            "cn.ntp.org.cn",
            "cn.pool.ntp.org",
            "s1a.time.edu.cn",
            "ntp.sjtu.edu.cn",
        };

        int i = 0;
        for (; i < ntp_count; i++)
        {
            hostent_content = gethostbyname(ntp_hosts[i]);
            if (hostent_content == NULL)
            {
                rtc_log("gethostbyname(%s)", ntp_hosts[i]);
                continue;
            }
            pptr = hostent_content->h_addr_list;
            ipp.s_addr = *(uint32_t *)(*pptr);
            err = sntp_get_time(&ipp, &current_time);
            if (err == kNoErr)
            {
                break;
            }
        }
    }

    if (err != kNoErr)
    {
        rtc_log("sntp_get_time4 err[%d]", err);
        return err;
    }

    mico_utc_time_ms_t utc_time_ms = (uint64_t)current_time.seconds * (uint64_t)1000
        + (current_time.microseconds / 1000);
    mico_time_set_utc_time_ms(&utc_time_ms);
    return kNoErr;
}

OSStatus UserRtcInit(void)
{
    OSStatus err = kNoErr;

    /* start rtc client */
    err = mico_rtos_create_thread(NULL, MICO_APPLICATION_PRIORITY, "rtc",
                                   (mico_thread_function_t) RtcThread,
                                   0x800, 0);
    require_noerr_string(err, exit, "ERROR: Unable to start the rtc thread.");

    if (kNoErr != err) rtc_log("ERROR1, app thread exit err: %d kNoErr[%d]", err, kNoErr);

    exit:
    return err;
}

void RtcThread(mico_thread_arg_t arg)
{
    OSStatus err = kUnknownErr;
    LinkStatusTypeDef LinkStatus;
//    mico_rtc_time_t rtc_time;

    mico_utc_time_t utc_time;
    mico_utc_time_t utc_time_last = 0;
    uint32_t boot_retry=0;
    while (1)
    {   //涓婄數鍚庤繛鎺ヤ簡wifi鎵嶅紑濮嬭蛋鏃跺惁鍒欑瓑寰呰繛鎺�
        micoWlanGetLinkStatus(&LinkStatus);
        if (LinkStatus.is_connected == 1 && (int32_t)(mico_rtos_get_time()-boot_retry)>=0)
        {
            err = UserSntpGetTime();
            boot_retry=mico_rtos_get_time()+60000u;
            if (err == kNoErr)
            {
                rtc_log("sntp success!");
                rtc_init = 1;
                break;
            }
        }
        mico_rtos_thread_sleep(3);
    }

    uint32_t next_sync=mico_rtos_get_time()+3600000u;
    while(1) {
        mico_time_get_utc_time(&utc_time);
        if(utc_time_last!=utc_time){utc_time_last=utc_time;total_time++;}
        uint32_t now=mico_rtos_get_time();
        if((int32_t)(now-next_sync)>=0){
            micoWlanGetLinkStatus(&LinkStatus);
            err=LinkStatus.is_connected?UserSntpGetTime():kConnectionErr;
            if(err==kNoErr)rtc_init=1;
            else rtc_init=2;
            next_sync=mico_rtos_get_time()+(err==kNoErr?3600000u:60000u);
        }
        mico_rtos_thread_msleep(1000);
    }

//  exit:
    rtc_log("EXIT: rtc exit with err = %d.", err);
    mico_rtos_delete_thread(NULL);
}

