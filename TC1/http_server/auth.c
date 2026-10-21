/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "mico.h"
#include "auth.h"
#include "../../common/httpd/http.h"
#include <string.h>
static char auth_user[33], auth_password[33];
static bool initialized;
int httpd_auth_init(const char *user, const char *password)
{
    if (!user || !password || strlen(user)>=sizeof(auth_user) ||
        strlen(password)>=sizeof(auth_password)) return kParamErr;
    /* Configuration updates run in task context. No socket operations or
     * allocation while scheduling is locked; readers see a complete pair. */
    mico_rtos_suspend_all_thread();
    strcpy(auth_user,user);strcpy(auth_password,password);initialized=true;
    mico_rtos_resume_all_thread();
    return kNoErr;
}
bool tc1_http_authorized(const char *authorization)
{
    char user[33],password[33];
    mico_rtos_suspend_all_thread();
    bool ready=initialized;
    memcpy(user,auth_user,sizeof(user));memcpy(password,auth_password,sizeof(password));
    mico_rtos_resume_all_thread();
    return ready && ph_http_basic(authorization,user,password);
}
