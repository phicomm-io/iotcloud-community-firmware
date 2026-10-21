#include "http_server/web_log.h"

#include "mico.h"
#include "ota_server.h"  // 使用相对路径，因为ota_server.c和ota_server.h在同一目录下
#include "main.h"
#include "mqtt_server/user_mqtt_client.h"
#include "../../common/json_number_checked.h"

float ota_progress = -2;

static void OtaServerStatusHandler(OTA_STATE_E state, float progress)
{
    char str[64] = { 0 };
    switch (state)
    {
        case OTA_LOADING:
            ota_progress = progress;
            /* Formatting failure may suppress a log, never the OTA callback. */
            if (json_number_format_fixed(str,sizeof(str),progress,2) >= 0)
                ota_log("ota server is loading, progress %s%%",str);
            str[0] = 0;
            if (((int) progress)%10 == 1)
                sprintf(str, "{\"mac\":\"%s\",\"ota_progress\":%d}", str_mac,((int) progress));
            break;
        case OTA_SUCCE:
            ota_progress = 100;
            ota_log("ota server daemons success");
            sprintf(str, "{\"mac\":\"%s\",\"ota_progress\":100}", str_mac);
            break;
        case OTA_FAIL:
            ota_progress=-1;
            ota_log("ota server daemons failed");
            sprintf(str, "{\"mac\":\"%s\",\"ota_progress\":-1}", str_mac);
            break;
        default:
            break;
    }
}

void UserOtaStart(char *url, char *md5)
{
    ota_progress = 0;
    ota_log("ready to ota:%s",url);
    ota_server_start(url, md5, OtaServerStatusHandler);
}