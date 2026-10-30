#pragma once
#include "os_host.h"
#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
typedef int OSStatus;
typedef int lfs_t;
typedef long lfs_ssize_t;
typedef long lfs_soff_t;
typedef struct {FILE *file;} lfs_file_t;
enum {LFS_O_RDONLY=1,LFS_O_WRONLY=2,LFS_O_CREAT=4,LFS_O_TRUNC=8};
#define UART_PORT_INDEX_0 0
#define UART_PORT_INDEX_1 1
extern lfs_t lfs;
extern char store_dir[1024];
extern int mqtt_port;
bool Main_IsConnectedToWiFi(void);
bool Main_IsOpenAccessPointMode(void);
const char *HAL_GetMyIPString(void);
int HAL_GetWifiStrength(void);
void WiFI_GetMacAddress(char *);
void HAL_PIN_Setup_Output_Initial(int,int);
void HAL_PIN_Setup_Input_Pullup(int);
void HAL_PIN_SetOutputValue(int,int);
int HAL_PIN_ReadDigitalInput(int);
void HAL_RebootModule(void);
int lfs_present(void);
void init_lfs(int);
int lfs_file_open(lfs_t *,lfs_file_t *,const char *,int);
lfs_ssize_t lfs_file_write(lfs_t *,lfs_file_t *,const void *,size_t);
lfs_ssize_t lfs_file_read(lfs_t *,lfs_file_t *,void *,size_t);
lfs_soff_t lfs_file_size(lfs_t *,lfs_file_t *);
int lfs_file_sync(lfs_t *,lfs_file_t *);
int lfs_file_close(lfs_t *,lfs_file_t *);
int lfs_rename(lfs_t *,const char *,const char *);
const char *CFG_GetWebPassword(void);
void CFG_SetWebPassword(const char *);
void CFG_SetDefaultConfig(void);
void CFG_SetDeviceName(const char *);
const char *CFG_GetWiFiSSID(void);
const char *CFG_GetWiFiSSID2(void);
const char *CFG_GetWiFiPass(void);
void CFG_SetWiFiSSID(const char *);
void CFG_SetWiFiPass(const char *);
void CFG_SetWiFiSSID2(const char *);
void CFG_SetWiFiPass2(const char *);
const char *CFG_GetMQTTHost(void);
const char *CFG_GetMQTTUserName(void);
const char *CFG_GetMQTTPass(void);
int CFG_GetMQTTPort(void);
void CFG_SetMQTTHost(const char *);
void CFG_SetMQTTUserName(const char *);
void CFG_SetMQTTPass(const char *);
void CFG_SetMQTTPort(int);
void CFG_Save_IfThereArePendingChanges(void);
void set_printf_port(unsigned char);
void host_uart_feed(int,const unsigned char *,int);
void UART_InitReceiveRingBufferEx(int,int);
int UART_GetReceiveRingBufferSizeEx(int);
int UART_InitUARTEx(int,int,int,bool);
int UART_GetDataSizeEx(int);
unsigned char UART_GetByteEx(int,int);
void UART_ConsumeBytesEx(int,int);
int OTA_GetProgress(void);
int OTA_GetTotalBytes(void);
struct host_scan {unsigned char ssid[32];int level,security;};
struct scanu_rst_upload {struct host_scan *res[20];};
void bk_wlan_scan_ap_reg_cb(void (*)(void *,uint8_t));
void bk_wlan_start_scan(void);
struct scanu_rst_upload *sr_get_scan_results(void);
int bk_wlan_get_scan_ap_result_numbers(void);
void sr_release_scan_results(struct scanu_rst_upload *);
