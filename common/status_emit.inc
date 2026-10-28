/* Direct binary sink for the frozen schema; no graphs or formatting. */
typedef ph_writer status_sink;
static void emit_begin(status_sink *s,const char *key,bool array){phw_begin(s,key,array);}
static void emit_end(status_sink *s){phw_end(s);}
static void emit_string(status_sink *s,const char *key,const char *text){phw_text(s,key,text);}
static void emit_int(status_sink *s,const char *key,int32_t n){phw_i32(s,key,n);}
static void emit_bool(status_sink *s,const char *key,bool value){phw_bool(s,key,value);}
static void emit_null(status_sink *s,const char *key){phw_null(s,key);}
static void emit_double(status_sink *s,const char *key,double value){phw_number(s,key,value);}
static void variant_fields(status_sink *o)
{
#define STATUS_INT(k,v) emit_int(o,k,v)
#define STATUS_BOOL(k,v) emit_bool(o,k,v)
#define STATUS_STRING(k,v) emit_string(o,k,v)
#include "variant_fields.inc"
#undef STATUS_INT
#undef STATUS_BOOL
#undef STATUS_STRING
}
#if defined(PHICOMM_APPLICATION_WATCHDOG)
static void watchdog_fields(status_sink *o,const watchdog_status_t *s)
{
#define STATUS_INT(k,v) emit_int(o,k,v)
#define STATUS_BOOL(k,v) emit_bool(o,k,v)
#define STATUS_STRING(k,v) emit_string(o,k,v)
#include "watchdog_fields.inc"
#undef STATUS_INT
#undef STATUS_BOOL
#undef STATUS_STRING
}
#endif
