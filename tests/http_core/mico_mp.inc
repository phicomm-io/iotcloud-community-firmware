
#define kNoErr 0
#define MODEL_JSON_LIMIT 8192
typedef struct {char web_password[33];} app_config_t;
static void app_config_get(app_config_t*c){strcpy(c->web_password,password);}
static bool app_ota_begin(uint32_t n){assert(n);ota_begins++;job=true;return true;}
static bool app_ota_begin_unsigned(uint32_t n){return app_ota_begin(n);}
static void app_ota_abort(const char*r);
static bool app_ota_write(const void*p,size_t n){assert(p&&n<=512);written+=n;if(fail_write){app_ota_abort("write failed");return false;}return true;}
static bool app_ota_finish(void){finishes++;job=false;return true;}
static void app_ota_abort(const char*r){assert(r);aborts++;job=false;}
#define OSStatus int
