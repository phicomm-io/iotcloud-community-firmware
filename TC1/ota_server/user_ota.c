#include "http_server/web_log.h"

#include "mico.h"
#include "ota_server.h"  // 使用相对路径，因为ota_server.c和ota_server.h在同一目录下
#include "main.h"
#include "mqtt_server/user_mqtt_client.h"
#include "../../common/a1_decimal.h"

float ota_progress = -2;

static void OtaServerStatusHandler(OTA_STATE_E state, float progress)
{
    char str[32];
    switch (state)
    {
        case OTA_LOADING:
            ota_progress = progress;
            /* Formatting failure may suppress a log, never the OTA callback. */
            if (ph_decimal_format_float32(str,sizeof(str),progress) >= 0)
                ota_log("ota server is loading, progress %s%%",str);
            break;
        case OTA_SUCCE:
            ota_progress = 100;
            ota_log("ota server daemons success");
            break;
        case OTA_FAIL:
            ota_progress=-1;
            ota_log("ota server daemons failed");
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