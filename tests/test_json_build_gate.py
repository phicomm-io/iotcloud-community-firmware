"""Threaded actual-production graph exclusion; no socket or broker traffic."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from tests.sdk_json import copy_patched_json
from tests.test_status_views import function

ROOT=Path(__file__).resolve().parents[1]

class JsonBuildGate(unittest.TestCase):
    def test_production_threads_and_callbacks(self):
        sdk=Path(os.environ.get('PHICOMM_TEST_JSON_DIR',ROOT/'build/a1-sdk/mico-os/libraries/utilities/json_c'))
        with tempfile.TemporaryDirectory() as directory:
            temp=Path(directory);json=copy_patched_json(sdk,temp/'sdk')
            for path in json.glob('*.c'):path.write_text(path.read_text().replace('%ld','%d'))
            header=(ROOT/'tests/stubs/mico.h').read_text()
            start=header.index('static inline int mico_rtos_init_mutex(')
            end=header.index('static inline uint32_t mico_rtos_get_time(',start)
            header=header[:start]+'''static inline int mico_rtos_init_mutex(mico_mutex_t*m){return pthread_mutex_init(m,NULL);}
static inline void mico_rtos_lock_mutex(mico_mutex_t*m){assert(!pthread_mutex_lock(m));}
static inline void mico_rtos_unlock_mutex(mico_mutex_t*m){assert(!pthread_mutex_unlock(m));}
'''+header[end:]
            header=header.replace('typedef unsigned mico_mutex_t;','#include <pthread.h>\ntypedef pthread_mutex_t mico_mutex_t;')
            (temp/'mico.h').write_text(header)
            # Inject single-allocation failure through the pinned real allocator
            # permit callback, while retaining the actual service floor policy.
            source=(ROOT/'tests/http_core/json_gate.c').read_text()
            mqtt=(ROOT/'common/mqtt/service.c').read_text()
            telemetry='static bool service_mqtt_capabilities_pending;\n'+'\n'.join(function(mqtt,n) for n in (
                'static struct printbuf *service_mqtt_wire(', 'static int service_mqtt_publish(',
                'static int service_mqtt_try_capabilities(', 'int service_mqtt_snapshot(', 'int service_mqtt_capabilities('))
            source=source.replace('/* PRODUCTION_MQTT_TELEMETRY */',telemetry)
            source=source.replace('static int test_gate_permit(size_t n)', 'int test_gate_permit(size_t n)')
            (temp/'probe.c').write_text(source)
            heap=json/'json_heap.c';text=heap.read_text()
            text=text.replace('if (!json_heap_permit(', 'if (!test_gate_permit(')
            heap.write_text('#include <stddef.h>\nint test_gate_permit(size_t);\n'+text)
            binary=temp/'probe'
            bear=ROOT/'build/ota-tls/bearssl-0.6'
            hash_sources=[str(bear/'src'/p) for p in ('hash/sha2small.c','codec/dec32be.c','codec/enc32be.c','codec/enc64be.c')]
            subprocess.run([os.environ.get('CC','cc'),'-std=gnu99','-g','-pthread',
                '-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie',
                '-I'+str(bear/'inc'),'-I'+str(bear/'src'),'-I'+str(temp),'-I'+str(ROOT),'-I'+str(ROOT/'tests/stubs'),'-I'+str(json),
                str(temp/'probe.c'),str(ROOT/'common/httpd/product.c'),str(ROOT/'common/httpd/core.c'),
                str(ROOT/'common/httpd/parser.c'),str(ROOT/'common/json_owned.c'),
                *hash_sources,*map(str,json.glob('*.c')),'-o',str(binary)],check=True,timeout=60)
            subprocess.run([str(binary)],check=True,timeout=30)
