#include "platform_host.h"
#include "factory_mac.h"
#include "../protocol/httpd/http.h"
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
lfs_t lfs;
/* Unit tests omit the socket service; network tests link its real snapshot. */
__attribute__((weak)) ph_http_diagnostics ph_http_snapshot(void){ph_http_diagnostics d={0};return d;}
int mounted;
char store_dir[1024];
char mqtt_host[128]="127.0.0.1",mqtt_user[65]="test-user",mqtt_pass[128]="test-pass",web_password[33]="secret";
char wifi_ssid[33]="lab",wifi_pass[64]="test-password",wifi_ssid2[33],wifi_pass2[64],device_name[64]="S30-V1B";
int mqtt_port;
uint32_t rtos_get_time(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint32_t)((uint64_t)t.tv_sec*1000+t.tv_nsec/1000000);}
int rtos_init_mutex(beken_mutex_t *m){return pthread_mutex_init(m,NULL);}
void rtos_lock_mutex(beken_mutex_t *m){pthread_mutex_lock(m);}
void rtos_unlock_mutex(beken_mutex_t *m){pthread_mutex_unlock(m);}
typedef struct {void (*fn)(beken_thread_arg_t);beken_thread_arg_t arg;} thread_start;
static void *start_thread(void *p){thread_start s=*(thread_start*)p;free(p);s.fn(s.arg);return NULL;}
int rtos_create_thread(beken_thread_t *handle,int priority,const char *name,void (*fn)(beken_thread_arg_t),int stack,beken_thread_arg_t arg){
 (void)priority;(void)name;(void)stack;pthread_t t;thread_start *s=malloc(sizeof(*s));if(!s)return -1;*s=(thread_start){fn,arg};
 int rc=pthread_create(handle?handle:&t,NULL,start_thread,s);if(rc)free(s);else pthread_detach(handle?*handle:t);return rc;
}
void rtos_delete_thread(beken_thread_t *thread){(void)thread;pthread_exit(NULL);}
void rtos_delay_milliseconds(unsigned ms){usleep(ms*1000u);}
/* Measured idle DUT heap. An inherited 16 KiB MQTT reserve must fail here. */
size_t xPortGetFreeHeapSize(void){return 13144;}
int ph_host_http_port;
bool Main_IsOpenAccessPointMode(void){return false;}
bool Main_IsConnectedToWiFi(void){return true;}
const char *HAL_GetMyIPString(void){return "127.0.0.1";}
int PH_BL_CopyLocalIPAddress(char *out,size_t capacity){
 const char *ip=HAL_GetMyIPString();size_t n=strlen(ip);
 if(!out||n>=capacity)return 0;memcpy(out,ip,n+1);return 1;
}
int HAL_GetWifiStrength(void){return -42;}
void WiFI_GetMacAddress(char *out){const unsigned char mac[]={0xaa,0xbb,0xcc,0xdd,0xee,0xff};memcpy(out,mac,6);}
const char *PH_BL_HostName(void){static char name[32];unsigned char mac[6];WiFI_GetMacAddress((char*)mac);ph_mac_host_name(name,mac);return name;}
void HAL_PIN_Setup_Output_Initial(int pin,int value){(void)pin;(void)value;}
void HAL_PIN_Setup_Input_Pullup(int pin){(void)pin;}
void HAL_PIN_SetOutputValue(int pin,int value){(void)pin;(void)value;}
int HAL_PIN_ReadDigitalInput(int pin){(void)pin;return 1;}
void HAL_RebootModule(void){_Exit(0);}
int lfs_present(void){return mounted;}
void init_lfs(int create){(void)create;mounted=1;}
int lfs_file_open(lfs_t *fs,lfs_file_t *f,const char *name,int flags){(void)fs;char path[1200];snprintf(path,sizeof(path),"%s/%s",store_dir,name);f->file=fopen(path,flags&LFS_O_WRONLY?"wb":"rb");return f->file?0:-1;}
lfs_ssize_t lfs_file_write(lfs_t *fs,lfs_file_t *f,const void *p,size_t n){(void)fs;return (lfs_ssize_t)fwrite(p,1,n,f->file);}
lfs_ssize_t lfs_file_read(lfs_t *fs,lfs_file_t *f,void *p,size_t n){(void)fs;return (lfs_ssize_t)fread(p,1,n,f->file);}
lfs_soff_t lfs_file_size(lfs_t *fs,lfs_file_t *f){(void)fs;long at=ftell(f->file);fseek(f->file,0,SEEK_END);long n=ftell(f->file);fseek(f->file,at,SEEK_SET);return n;}
int lfs_file_sync(lfs_t *fs,lfs_file_t *f){(void)fs;return fflush(f->file);}
int lfs_file_close(lfs_t *fs,lfs_file_t *f){(void)fs;return fclose(f->file);}
int lfs_rename(lfs_t *fs,const char *a,const char *b){(void)fs;char src[1200],dst[1200];snprintf(src,sizeof(src),"%s/%s",store_dir,a);snprintf(dst,sizeof(dst),"%s/%s",store_dir,b);return rename(src,dst);}
const char *CFG_GetWebPassword(void){return web_password;}
int PH_BL_WebPasswordSet(const char *);
void PH_BL_SetWebPasswordMemory(const char *p){snprintf(web_password,sizeof(web_password),"%s",p);}
void CFG_SetWebPassword(const char *p){(void)PH_BL_WebPasswordSet(p);}
void CFG_SetDefaultConfig(void){mqtt_host[0]=mqtt_user[0]=mqtt_pass[0]=web_password[0]=0;}
void CFG_SetDeviceName(const char *p){snprintf(device_name,sizeof(device_name),"%s",p);}
const char *CFG_GetWiFiSSID(void){return wifi_ssid;}
const char *CFG_GetWiFiSSID2(void){return wifi_ssid2;}
const char *CFG_GetWiFiPass(void){return wifi_pass;}
void CFG_SetWiFiSSID(const char *p){snprintf(wifi_ssid,sizeof(wifi_ssid),"%s",p);}
void CFG_SetWiFiPass(const char *p){snprintf(wifi_pass,sizeof(wifi_pass),"%s",p);}
void CFG_SetWiFiSSID2(const char *p){snprintf(wifi_ssid2,sizeof(wifi_ssid2),"%s",p);}
void CFG_SetWiFiPass2(const char *p){snprintf(wifi_pass2,sizeof(wifi_pass2),"%s",p);}
const char *CFG_GetMQTTHost(void){return mqtt_host;}
const char *CFG_GetMQTTUserName(void){return mqtt_user;}
const char *CFG_GetMQTTPass(void){return mqtt_pass;}
int CFG_GetMQTTPort(void){return mqtt_port;}
void CFG_SetMQTTHost(const char *p){snprintf(mqtt_host,sizeof(mqtt_host),"%s",p);}
void CFG_SetMQTTUserName(const char *p){snprintf(mqtt_user,sizeof(mqtt_user),"%s",p);}
void CFG_SetMQTTPass(const char *p){snprintf(mqtt_pass,sizeof(mqtt_pass),"%s",p);}
void CFG_SetMQTTPort(int p){mqtt_port=p;}
void CFG_Save_IfThereArePendingChanges(void){}
static struct {unsigned char bytes[512];int used,capacity;} uart[2];
static unsigned char printf_port=2;
void set_printf_port(unsigned char port){assert(port==1);printf_port=port;}
void host_uart_feed(int port,const unsigned char *p,int n){assert(port>=0&&port<2&&n>=0&&uart[port].used+n<=512);memcpy(uart[port].bytes+uart[port].used,p,(size_t)n);uart[port].used+=n;}
void UART_InitReceiveRingBufferEx(int port,int n){assert(port>=0&&port<2&&n==512);uart[port].capacity=n;uart[port].used=0;}
int UART_GetReceiveRingBufferSizeEx(int port){assert(port>=0&&port<2);return uart[port].capacity;}
int UART_InitUARTEx(int port,int baud,int parity,bool flow){assert(printf_port==1&&port==1&&uart[port].capacity==512&&baud==4800&&parity==2&&!flow);return 0;}
int UART_GetDataSizeEx(int port){assert(port>=0&&port<2);return uart[port].used;}
unsigned char UART_GetByteEx(int port,int i){assert(port>=0&&port<2&&i>=0&&i<uart[port].used);return uart[port].bytes[i];}
void UART_ConsumeBytesEx(int port,int n){assert(port>=0&&port<2&&n>=0&&n<=uart[port].used);uart[port].used-=n;memmove(uart[port].bytes,uart[port].bytes+n,(size_t)uart[port].used);}

int PH_BL_MqttTlsEnabled(void){return 0;}
int OTA_GetProgress(void){return -1;}
int OTA_GetTotalBytes(void){return 0;}
void OTA_RequestDownloadFromHTTP(const char *s){(void)s;}
void bk_wlan_scan_ap_reg_cb(void (*cb)(void *,uint8_t)){(void)cb;}
void bk_wlan_start_scan(void){}
struct scanu_rst_upload *sr_get_scan_results(void){return NULL;}
int bk_wlan_get_scan_ap_result_numbers(void){return 0;}
void sr_release_scan_results(struct scanu_rst_upload *s){(void)s;}
OSStatus BkRandomNumberRead(void *out,int n){assert(n>0&&n<=4);FILE *f=fopen("/dev/urandom","rb");if(!f)return -1;bool ok=fread(out,1,(size_t)n,f)==(size_t)n;fclose(f);return ok?0:-1;}
