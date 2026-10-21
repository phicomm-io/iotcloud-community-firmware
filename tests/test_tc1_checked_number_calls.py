"""Exercise the actual OTA callback when checked log formatting is unavailable."""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from tests.test_tc1_mqtt_stack import extract

ROOT = Path(__file__).resolve().parents[1]


class Tc1CheckedNumberCalls(unittest.TestCase):
    def test_ota_progress_does_not_depend_on_log_allocation(self):
        callback = extract((ROOT/'TC1/ota_server/user_ota.c').read_text(),
                           'static void OtaServerStatusHandler(')
        source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
typedef enum {OTA_LOADING,OTA_SUCCE,OTA_FAIL} OTA_STATE_E;
static float ota_progress=-2;
static char str_mac[]="aabbccddeeff",line[128];
static unsigned logs;
static bool fail;
static int json_number_format_fixed(char *out,unsigned size,double value,unsigned precision){
 if(fail)return -1;
 return snprintf(out,size,"%.*f",precision,value);
}
static void record_log(const char *format,...){
 va_list args;va_start(args,format);vsnprintf(line,sizeof(line),format,args);va_end(args);logs++;
}
#define ota_log(...) record_log(__VA_ARGS__)
'''+callback+r'''
int main(void){
 OtaServerStatusHandler(OTA_LOADING,21.125f);
 assert(ota_progress==21.125f&&logs==1&&!strcmp(line,"ota server is loading, progress 21.12%"));
 fail=true;OtaServerStatusHandler(OTA_LOADING,31.125f);assert(ota_progress==31.125f&&logs==1);
 OtaServerStatusHandler(OTA_SUCCE,100);assert(ota_progress==100&&logs==2);
 OtaServerStatusHandler(OTA_FAIL,0);assert(ota_progress==-1&&logs==3);
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp);(path/'probe.c').write_text(source)
            subprocess.run([os.environ.get('CC','cc'),'-std=c99','-Wall','-Wextra','-Werror',
                '-fsanitize=address,undefined','-no-pie',str(path/'probe.c'),'-o',str(path/'probe')],check=True)
            subprocess.run([str(path/'probe')],check=True,timeout=10)

    def test_application_float_format_calls_use_checked_api(self):
        # Comments may contain old examples; inspect live format literals only.
        for filename in ('TC1/mqtt_server/user_mqtt_client.c','TC1/ota_server/user_ota.c'):
            source=(ROOT/filename).read_text()
            source=re.sub(r'/\*.*?\*/|//[^\n]*','',source,flags=re.S)
            self.assertIsNone(re.search(r'%(?:[0-9.#+ -]*)(?:[fFeEgG])',source),filename)


if __name__=='__main__':
    unittest.main()
