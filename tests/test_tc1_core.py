"""Host regression coverage for real TC1 scheduling and fragmented HTTP bodies."""
import pathlib, subprocess, tempfile, unittest, os
ROOT=pathlib.Path(__file__).resolve().parents[1]

def execute(source):
    with tempfile.TemporaryDirectory() as d:
        p=pathlib.Path(d);(p/'test.c').write_text(source)
        subprocess.run(['cc','-std=c99','-Wall','-Wextra','-Wno-misleading-indentation','-fsanitize=address,undefined','-I',str(ROOT/'TC1/http_server'),str(p/'test.c'),'-o',str(p/'test')],check=True)
        subprocess.run([str(p/'test')],check=True,timeout=8,env={**os.environ,"UBSAN_OPTIONS":"halt_on_error=1"})

class Core(unittest.TestCase):
    def test_tasks_empty_delete_rebuild_capacity_and_atomic_replacement(self):
        src=(ROOT/'TC1/timed_task/timed_task.c').read_text()
        funcs=src[src.index('static const int day_sec'):src.index('void ProcessTask()')]+src[src.index('char *GetTaskStr('):]
        execute(r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#define MAX_TASK_NUM 128
#define kNoErr 0
typedef int mico_mutex_t;
static int locked;
static int mico_rtos_init_mutex(int *m){*m=0;return 0;}
static void mico_rtos_lock_mutex(int*m){(void)m;if(locked)abort();locked=1;}
static void mico_rtos_unlock_mutex(int*m){(void)m;locked=0;}
static int mico_system_context_update(void *p){(void)p;return 0;}
static void *sys_config;
struct TimedTask {bool on_use;time_t prs_time;int operation,on,weekday;struct TimedTask *next;};
typedef struct TimedTask *pTimedTask;
static struct {pTimedTask task_top;int task_count;struct TimedTask timed_tasks[128];} config;
#define user_config (&config)
'''+funcs+r'''
#include <assert.h>
int main(void) {
 TasksInit();char *s=GetTaskStr();assert(!strcmp(s,"[]"));free(s);
 assert(!AddTaskValue(1,99,1,0));assert(config.task_count==0);
 for(int i=0;i<128;++i)assert(AddTaskValue(2000000000+i,1,1,0));
 assert(!AddTaskValue(2000000999,1,1,0));assert(config.task_count==128);
 assert(DelTask(2000000002));assert(config.task_count==127);
 s=GetTaskStr();assert(strstr(s,"2000000001")&&strstr(s,"2000000003")&&!strstr(s,"2000000002"));free(s);
 for(int i=0;i<128;++i)config.timed_tasks[i].next=(void*)1;
 config.task_top=(void*)1;TasksInit();assert(config.task_count==127);
 struct TimedTask snapshot[128];assert(TaskSnapshot(snapshot,128)==127);
 assert(!TasksReplace(snapshot,129));assert(config.task_count==127);
 assert(TasksReplace(NULL,0));s=GetTaskStr();assert(!strcmp(s,"[]"));free(s);
 assert(AddTaskValue(2000000000,11,0,8));assert(config.task_count==1);
}
''')
    def test_due_task_execution_led_persistence_and_missed_repeat(self):
        src=(ROOT/'TC1/timed_task/timed_task.c').read_text()
        func=src[src.index('void ProcessTask()'):src.index('char *GetTaskStr(')]
        execute(r"""
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <time.h>
#include <assert.h>
#define SWITCH_ALL_SOCKETS 0
#define SWITCH_SOCKET_1 1
#define SWITCH_SOCKET_2 2
#define SWITCH_SOCKET_3 3
#define SWITCH_SOCKET_4 4
#define SWITCH_SOCKET_5 5
#define SWITCH_SOCKET_6 6
#define SWITCH_LED_ENABLE 7
#define REBOOT_SYSTEM 8
#define CONFIG_WIFI 9
#define RESET_SYSTEM 10
#define SWITCH_CHILD_LOCK_ENABLE 11
#define REBOOT_HTTP 12
#define REBOOT_SOFT 13
#define SOCKET_NUM 6
struct TimedTask {bool on_use;time_t prs_time;int operation,on,weekday;struct TimedTask *next;};
typedef struct TimedTask *pTimedTask;
static struct {pTimedTask task_top;int power_led_enabled,user[1];} config;
#define user_config (&config)
static int task_lock,childLockEnabled,actions,last_on;
static void *sys_config;
static const int day_sec=86400;
static time_t fake_time(time_t *p){(void)p;return 1000000;}
#define time fake_time
static void mico_rtos_lock_mutex(void*p){(void)p;assert(!task_lock);task_lock=1;}
static void mico_rtos_unlock_mutex(void*p){(void)p;task_lock=0;}
static void rebuild_tasks(void){if(!config.task_top->on_use)config.task_top=NULL;}
#define mico_system_context_update(p) ((void)(p))
#define task_log(...) ((void)0)
#define UserMqttSendSocketState(i) ((void)(i))
#define UserMqttSendTotalSocketState() ((void)0)
#define UserMqttSendLedState() ((void)0)
#define UserMqttSendChildLockState() ((void)0)
#define MicoSystemReboot() ((void)0)
#define UserSoftReboot() ((void)0)
#define AppHttpdStop() ((void)0)
#define AppHttpdStart() ((void)0)
#define mico_rtos_thread_sleep(i) ((void)(i))
#define micoWlanSuspendStation() ((void)0)
#define ApInit(i) ((void)(i))
#define mico_system_context_restore(p) ((void)(p))
static int RelayOut(void){return 1;}
static void UserLedSet(int on){last_on=on;++actions;}
static void UserRelaySet(int index,int on){(void)index;last_on=on;++actions;}
static void UserRelaySetAll(int on){last_on=on;++actions;}
"""+func+r"""
int main(void){
 ProcessTask();assert(actions==0);
 struct TimedTask t={true,1000001,1,0,0,NULL};config.task_top=&t;
 ProcessTask();assert(actions==0&&t.on_use);
 t.prs_time=999999;ProcessTask();assert(actions==1&&last_on==0&&!config.task_top);
 t=(struct TimedTask){true,1,7,1,8,NULL};config.task_top=&t;
 ProcessTask();assert(actions==2&&last_on==1&&config.power_led_enabled==1);
 assert(t.prs_time>1000000&&t.prs_time<=1000000+86400);
 ProcessTask();assert(actions==2);
 t=(struct TimedTask){true,1,11,1,0,NULL};config.task_top=&t;
 ProcessTask();assert(childLockEnabled==1&&config.user[0]==1&&!config.task_top);
}
""")
    def test_http_fragments_oversize_embedded_null_and_close(self):
        src=(ROOT/'TC1/http_server/app_httpd.c').read_text();func=src[src.index('static int read_headers('):src.index('static int response_json(')]
        execute(r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#define kNoErr 0
#define kParamErr -1
#define kNoMemoryErr -2
#define kGeneralErr -3
#define HTTPD_MAX_MESSAGE 512
typedef struct {int hdr_parsed,sock,chunked,remaining_bytes;const char *filename;} httpd_request_t;
static int ota_active;
static int ota_job_active(void){return ota_active;}
static const char *input;static int length,offset,closed;
static int httpd_parse_hdr_tags(httpd_request_t*r,int fd,char*b,int n){(void)fd;(void)b;(void)n;r->remaining_bytes=length;return 0;}
static int httpd_recv(int fd,char*b,int n,int flags){(void)fd;(void)flags;if(closed)return 0;int count=n>2?2:n;memcpy(b,input+offset,count);offset+=count;return count;}
'''+func+r'''
int main(void){
 char buf[8];httpd_request_t r={0};input="abcdef";length=6;assert(!read_body(&r,buf,8));assert(!strcmp(buf,"abcdef")&&offset==6);
 r=(httpd_request_t){0};length=8;offset=0;assert(read_body(&r,buf,8)<0&&offset==0);
 r=(httpd_request_t){0};length=3;input="a\0b";offset=0;assert(read_body(&r,buf,8)<0);
 r=(httpd_request_t){0};closed=1;assert(read_body(&r,buf,8)<0);
 r=(httpd_request_t){0};closed=0;length=0;assert(!read_body(&r,buf,8)&&buf[0]==0);
 ota_active=1;length=6;offset=0;input="abcdef";
 r=(httpd_request_t){.filename="/config"};assert(read_body(&r,buf,8)==kGeneralErr&&offset==0);
 r=(httpd_request_t){.filename="/api/v1/command"};assert(!read_body(&r,buf,8)&&offset==6);
}
''')
    def test_json_escaping_and_capacity(self):
        execute(r'''
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include "json_buffer.h"
int main(void){char data[64];JsonBuffer b={data,0,sizeof(data),true};jb_string(&b,"quote\" slash\\ line\n");assert(b.used==jb_string_size("quote\" slash\\ line\n")+2);assert(b.valid&&!strcmp(data,"\"quote\\\" slash\\\\ line\\u000a\""));char tiny[2];b=(JsonBuffer){tiny,0,2,true};jb_string(&b,"long");assert(!b.valid);}
''')
if __name__=='__main__':unittest.main()
