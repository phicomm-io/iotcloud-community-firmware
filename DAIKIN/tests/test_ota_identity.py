"""Exercise the production HTTP OTA handler with real P-256 test signatures.

Only ESP flash/HTTP APIs are stubbed. No production private key or device is used.
"""
import ctypes
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

from cryptography.hazmat.primitives.asymmetric import ec

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('ota_package', ROOT / 'tools/ota/package.py')
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)


class OtaIdentity(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='daikin-ota-identity-')
        d = Path(cls.tmp.name)
        cls.key = ec.generate_private_key(ec.SECP256R1())
        public = cls.key.public_key().public_numbers()
        raw = public.x.to_bytes(32, 'big') + public.y.to_bytes(32, 'big')
        (d / 'keyring.h').write_text(
            '#include <stdint.h>\n#define OTA_KEY_COUNT 1\n'
            'static const struct {uint32_t id; uint8_t public_key[64];} ota_keys[]={{1,{'
            + ','.join(map(str, raw)) + '}}};\n')
        (d / 'signed_ota.c').write_text((ROOT / 'common/ota/signed_ota.c').read_text())
        (d / 'sha.h').write_text('''#include <openssl/sha.h>
typedef SHA256_CTX SHA256Context;
static int SHA256Reset(SHA256Context *s){return !SHA256_Init(s);}
static int SHA256Input(SHA256Context *s,const uint8_t *p,size_t n){return !SHA256_Update(s,p,n);}
static int SHA256Result(SHA256Context *s,uint8_t *p){return !SHA256_Final(p,s);}
''')
        # Compile the actual handler so ordering, route selection and activation
        # assertions cover production code as well as the pure prefix parser.
        http = (ROOT / 'DAIKIN/src/http.inc').read_text()
        handler = http.split('static volatile bool otaActive=false;', 1)[1].split(
            'static esp_err_t infoHandler(', 1)[0]
        (d / 'handler.inc').write_text('static volatile bool otaActive=false;' + handler)
        # OTA tests isolate the signed-image pipeline from the separately tested
        # token table. Preserve the actual authorization decision and inject
        # an already validated bearer session at this boundary.
        decision='static bool authorized(httpd_req_t *req){'+http.split('static bool authorized(httpd_req_t *req){',1)[1].split('\n',1)[0]+'\n'
        (d / 'authorization.inc').write_text(r'''struct ph_auth_context{};static bool localAuthReady=true;static int localAuth;
static bool requestAuthorization(httpd_req_t *,char header[80]){const auto &value=requestHeaders["Authorization"];if(value.size()>=80)return false;memcpy(header,value.c_str(),value.size()+1);return true;}
static bool ph_auth_http_check(int*,const char*header,ph_auth_context*){return !strcmp(header,"Bearer valid-session");}
static esp_err_t jsonResponse(httpd_req_t *req,const char *status,const String &body){httpd_resp_set_status(req,status);httpd_resp_set_hdr(req,"Cache-Control","no-store");httpd_resp_set_type(req,"application/msgpack");return httpd_resp_send(req,body.c_str(),body.size());}
'''+decision)
        (d / 'options.inc').write_text('static esp_err_t optionsHandler(' + http.split(
            'static esp_err_t optionsHandler(', 1)[1].split('\n', 1)[0] + '\n')
        (d / 'harness.cpp').write_text(r'''
#include <algorithm>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <string>
#include <map>
#include <openssl/evp.h>
#include "firmware_identity.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
extern "C" {
#include "signed_ota.h"
}
#pragma GCC diagnostic pop
using std::min;
using String=std::string;
typedef int esp_err_t;
typedef uint32_t esp_ota_handle_t;
#define ESP_OK 0
#define OTA_WITH_SEQUENTIAL_WRITES 0xfffffffeU
struct esp_image_header_t {uint8_t bytes[24];};
struct esp_image_segment_header_t {uint8_t bytes[8];};
struct esp_app_desc_t {uint32_t magic,secure,reserved[2];char version[32],project_name[32];uint8_t rest[176];};
struct esp_partition_t {uint32_t size;};
struct httpd_req_t {const char *uri;int content_len;const uint8_t *body;int actual,received,chunk;};
static esp_partition_t partition={8192};
static esp_app_desc_t self={};
static uint8_t flashBytes[2*1024*1024];
static size_t written;
static size_t erasedTo,maximumErase,burst,maximumBurst;
static int sequential,sectorErases,yields;
static uint32_t rebootAt;
static bool configRecovery=false,webPending=false,webDone=true;
static int fault,statusCode,erases,writes,reads,ends,activations,aborts,preferencesWrites,bytesAtErase;
static std::string response;
static std::map<std::string,std::string> requestHeaders,responseHeaders;
static daikin::FirmwareIdentity daikin_target;
struct Prefs {bool signedImage=false;void putBool(const char *key,bool value){if(strcmp(key,"ota_signed"))abort();signedImage=value;preferencesWrites++;}};
static Prefs prefs;
static char webPassword[33]={};
static int authMux __attribute__((unused))=0;
#define portENTER_CRITICAL(mux) (void)(mux)
#define portEXIT_CRITICAL(mux) (void)(mux)
static uint32_t millis(){return 100;}
static void delay(unsigned milliseconds){if(milliseconds!=1)abort();yields++;burst=0;}
static void flashWork(size_t n){burst+=n;maximumBurst=std::max(maximumBurst,burst);}
static int httpd_req_get_hdr_value_str(httpd_req_t *,const char *name,char *out,size_t n){
  auto value=requestHeaders.find(name);
  if(value==requestHeaders.end()||value->second.size()>=n)return -1;
  memcpy(out,value->second.c_str(),value->second.size()+1);return ESP_OK;
}
static void httpd_resp_set_hdr(httpd_req_t *,const char *name,const char *value){responseHeaders[name]=value;}
static void httpd_resp_set_status(httpd_req_t *,const char *status){statusCode=atoi(status);}
static void httpd_resp_set_type(httpd_req_t *,const char *){}
static int httpd_resp_send(httpd_req_t *,const char *body,size_t n){response.assign(body?body:"",n);return ESP_OK;}
static int __attribute__((unused)) mbedtls_base64_decode(uint8_t *out,size_t capacity,size_t *length,const uint8_t *input,size_t n){
  if(n%4||n/4*3>capacity)return -1;
  int used=EVP_DecodeBlock(out,input,n);if(used<0)return -1;
  while(n&&input[n-1]=='='){--used;--n;}*length=used;return 0;
}
#include "authorization.inc"
static int httpd_req_recv(httpd_req_t *req,char *data,size_t n){
  int available=req->actual-req->received;
  if(available<=0)return -1;
  size_t take=min(n,(size_t)min(available,req->chunk));
  memcpy(data,req->body+req->received,take);req->received+=take;return (int)take;
}
static httpd_req_t *currentRequest;
static const esp_partition_t *esp_ota_get_next_update_partition(void *){return fault==8?nullptr:&partition;}
static const esp_app_desc_t *esp_ota_get_app_description(){return &self;}
static esp_err_t esp_ota_begin(const esp_partition_t *,size_t imageSize,esp_ota_handle_t *handle){
  erases++;bytesAtErase=currentRequest->received;if(fault==1)return -1;
  *handle=1;written=0;sequential=imageSize==OTA_WITH_SEQUENTIAL_WRITES;
  if(!sequential){
    erasedTo=(imageSize+4095)&~size_t(4095);maximumErase=erasedTo;
    if(erasedTo>sizeof flashBytes)return -1;
    memset(flashBytes,0xff,erasedTo);
  }
  return ESP_OK;
}
static esp_err_t esp_ota_write(esp_ota_handle_t,const uint8_t *data,size_t n){
  writes++;if(fault==2||written+n>sizeof flashBytes)return -1;
  // Model IDF's sequential-write contract on dirty NOR flash: erase only newly
  // touched 4 KiB sectors, then program 1->0 without erasing an earlier prefix.
  if(sequential){
    size_t end=(written+n+4095)&~size_t(4095);
    if(end>erasedTo){
      if(fault==9||(fault==10&&written>=4096))return -1;
      size_t amount=end-erasedTo;maximumErase=std::max(maximumErase,amount);
      memset(flashBytes+erasedTo,0xff,amount);erasedTo=end;sectorErases++;
    }
  }
  for(size_t i=0;i<n;i++)flashBytes[written+i]&=data[i];
  written+=n;flashWork(n);return ESP_OK;
}
static esp_err_t esp_partition_read(const esp_partition_t *,uint32_t offset,uint8_t *data,size_t n){
  reads++;if(fault==3||offset+n>written)return -1;
  memcpy(data,flashBytes+offset,n);
  if(fault==4)data[0]^=1;
  if(fault==7&&n==daikin::firmwarePrefixSize)data[320]^=1;
  flashWork(n);
  return ESP_OK;
}
static esp_err_t esp_ota_end(esp_ota_handle_t){ends++;return fault==5?-1:ESP_OK;}
static esp_err_t esp_ota_abort(esp_ota_handle_t){aborts++;return ESP_OK;}
static esp_err_t esp_ota_set_boot_partition(const esp_partition_t *){
  if(fault==6)return -1;
  activations++;return ESP_OK;
}
#include "handler.inc"
#include "options.inc"
static void initialize(int hub,int failure,int authorizedRequest){
  const daikin::FirmwareIdentity full={"BRY88AB151K","PHICOMM-DAIKIN-1:full"};
  const daikin::FirmwareIdentity bridge={"BRY88AB151K","PHICOMM-DAIKIN-1:hub"};
  daikin_target=hub?bridge:full;
  memset(&self,0,sizeof self);strcpy(self.project_name,"arduino-lib-builder");
  fault=failure;statusCode=erases=writes=reads=ends=activations=aborts=preferencesWrites=bytesAtErase=0;
  erasedTo=maximumErase=burst=maximumBurst=0;sequential=sectorErases=yields=0;
  memset(flashBytes,0xa5,sizeof flashBytes);
  written=rebootAt=0;response.clear();requestHeaders.clear();responseHeaders.clear();prefs.signedImage=false;
  configRecovery=webPending=false;webDone=true;otaActive=false;
  strcpy(webPassword,"device-password");
  std::string credentials=authorizedRequest?"admin:device-password":"admin:wrong-password";
  uint8_t encoded[128];EVP_EncodeBlock(encoded,reinterpret_cast<const uint8_t *>(credentials.data()),credentials.size());
  requestHeaders["Authorization"]=authorizedRequest?"Bearer valid-session":"Bearer invalid-session";
  // Even ordinary uploads carry the conversion headers. They must never relax
  // the running variant's identity check on either signed or unsigned route.
  requestHeaders["X-IoTCloud-Target-Variant"]="hub";
  requestHeaders["X-IoTCloud-Confirm"]="full-to-hub";
}
extern "C" int upload(const uint8_t *body,unsigned length,int hub,int unsignedImage,
                      int chunk,int declared,int failure,int authorizedRequest,unsigned capacity){
  initialize(hub,failure,authorizedRequest);partition.size=capacity;
  httpd_req_t req={unsignedImage?"/api/v1/ota/upload-unsigned":"/api/v1/ota/upload",
    declared<0?(int)length:declared,body,(int)length,0,chunk};
  currentRequest=&req;
  uploadHandler(&req);return statusCode;
}
extern "C" int convert(const uint8_t *body,unsigned length,int hub,int auth,
                       const char *target,const char *confirm,int recovery,int busy,
                       int chunk,int declared,int failure,unsigned capacity){
  initialize(hub,failure,auth==1);partition.size=capacity;
  if(auth==0){webPassword[0]=0;requestHeaders.erase("Authorization");}
  if(auth==3)requestHeaders.erase("Authorization");
  if(auth==4)requestHeaders["Authorization"]="Basic invalid";
  if(target)requestHeaders["X-IoTCloud-Target-Variant"]=target;else requestHeaders.erase("X-IoTCloud-Target-Variant");
  if(confirm)requestHeaders["X-IoTCloud-Confirm"]=confirm;else requestHeaders.erase("X-IoTCloud-Confirm");
  configRecovery=recovery;otaActive=busy==1;webPending=busy==2;webDone=busy!=3;rebootAt=busy==4?999:0;
  httpd_req_t req={"/api/v1/ota/convert",declared<0?(int)length:declared,body,(int)length,0,chunk};
  currentRequest=&req;
  convertHandler(&req);return statusCode;
}
extern "C" int flash_matches(const uint8_t *body,unsigned length){return written==length&&!memcmp(flashBytes,body,length);}
extern "C" const char *reply(){return response.c_str();}
extern "C" const char *response_header(const char *name){return responseHeaders[name].c_str();}
extern "C" void preflight(){httpd_req_t req={};req.uri="/api/v1/ota/convert";optionsHandler(&req);}
extern "C" int count(int which){
  const int values[]={erases,writes,reads,ends,activations,aborts,(int)rebootAt,(int)otaActive,preferencesWrites,(int)prefs.signedImage,bytesAtErase,sequential,yields,(int)maximumBurst,(int)maximumErase,sectorErases,(int)erasedTo};
  return values[which];
}
''')
        for source, name in [(d / 'signed_ota.c', 'signed'), (ROOT / 'common/ota/crypto.c', 'crypto')]:
            subprocess.run(['cc', '-std=c99', '-fPIC', '-O2', '-Wno-deprecated-declarations',
                            '-I' + str(d), '-I' + str(ROOT / 'common/ota'),
                            '-c', str(source), '-o', str(d / (name + '.o'))], check=True)
        subprocess.run(['c++', '-std=c++11', '-shared', '-fPIC', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-I' + str(ROOT / 'DAIKIN/include'), '-I' + str(ROOT / 'common/ota'),
                        str(d / 'harness.cpp'), str(d / 'signed.o'), str(d / 'crypto.o'),
                        '-lcrypto', '-o', str(d / 'test.so')], check=True)
        cls.lib = ctypes.CDLL(str(d / 'test.so'))
        cls.lib.upload.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_int,
                                   ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_uint]
        cls.lib.convert.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_int,
                                   ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_int,
                                   ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_uint]
        cls.lib.flash_matches.argtypes = [ctypes.c_void_p, ctypes.c_uint]
        cls.lib.reply.restype = ctypes.c_char_p
        cls.lib.response_header.argtypes = [ctypes.c_char_p]
        cls.lib.response_header.restype = ctypes.c_char_p

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    @staticmethod
    def image(variant):
        image = bytearray(1408)
        image[0] = 0xe9
        image[1] = 1
        struct.pack_into('<I', image, 28, len(image) - 32)
        struct.pack_into('<I', image, 32, 0xabcd5432)
        image[80:112] = b'arduino-lib-builder'.ljust(32, b'\0')
        image[288:320] = b'BRY88AB151K'.ljust(32, b'\0')
        image[320:352] = ('PHICOMM-DAIKIN-1:' + variant).encode().ljust(32, b'\0')
        return bytes(image)

    def signed(self, image, key=None):
        return package.sign(image, 'daikin', '1.0.0', 'a' * 40, key or self.key)

    def upload(self, image, variant='full', unsigned=False, chunk=1024, declared=-1, fault=0, allow=True,
               capacity=8192):
        status = self.lib.upload(image, len(image), variant == 'hub', unsigned, chunk, declared, fault, allow, capacity)
        self.assertEqual(self.lib.count(7), 0, 'OTA busy state leaked')
        if status != 202:
            self.assertEqual(self.lib.count(4), 0, 'Rejected upload activated a partition')
            self.assertEqual(self.lib.count(6), 0, 'Rejected upload scheduled reboot')
        return status

    def convert(self, image, variant='full', auth=1, target='hub', confirm='full-to-hub',
                recovery=False, busy=0, chunk=1024, declared=-1, fault=0, capacity=8192):
        status = self.lib.convert(image, len(image), variant == 'hub', auth,
                                  target.encode() if target is not None else None,
                                  confirm.encode() if confirm is not None else None,
                                  recovery, busy, chunk, declared, fault, capacity)
        self.assertEqual(self.lib.count(7), busy == 1, 'OTA busy state changed incorrectly')
        if status != 202:
            self.assertEqual(self.lib.count(4), 0, 'Rejected conversion activated a partition')
            self.assertEqual(self.lib.count(6), 999 if busy == 4 else 0)
            self.assertEqual(self.lib.count(8), 0, 'Rejected conversion persisted metadata')
        return status

    def test_same_variant_signed_and_unsigned(self):
        for variant in ('full', 'hub'):
            for unsigned in (False, True):
                image = self.image(variant)
                data = image if unsigned else self.signed(image)
                for chunk in (1, 7, 191, 192, 353, 1024):
                    with self.subTest(variant=variant, unsigned=unsigned, chunk=chunk):
                        self.assertEqual(self.upload(data, variant, unsigned, chunk), 202)
                        self.assertEqual(self.lib.count(4), 1)
                        self.assertGreater(self.lib.count(2), 0)

    def test_cross_variant_rejected_for_both_routes(self):
        for source, target in (('full', 'hub'), ('hub', 'full')):
            for unsigned in (False, True):
                image = self.image(source)
                self.assertEqual(self.upload(image if unsigned else self.signed(image), target, unsigned), 400)

    def test_missing_noncanonical_unknown_or_relocated_identity(self):
        for variant in ('full', 'hub'):
            for label in (bytes(32), b'PHICOMM-DAIKIN-1:other'.ljust(32, b'\0'),
                          ('PHICOMM-DAIKIN-1:' + variant).encode().ljust(31, b'\0') + b'x'):
                image = bytearray(self.image(variant))
                image[400:432] = image[320:352]  # A substring elsewhere cannot rescue the identity.
                image[320:352] = label
                for unsigned in (False, True):
                    self.assertEqual(self.upload(bytes(image) if unsigned else self.signed(image), variant, unsigned), 400)

    def test_untrusted_signature_and_tampered_digest_cannot_activate(self):
        image = self.image('full')
        invalid = bytearray(self.signed(image)); invalid[128:192] = bytes(64)
        for data in (bytes(invalid), self.signed(image, ec.generate_private_key(ec.SECP256R1())), image):
            self.assertEqual(self.upload(data), 400)
            self.assertEqual(self.lib.count(0), 0, 'Unauthenticated envelope erased flash')
        # Replace hub's label with full's without re-signing the payload digest.
        forged = bytearray(self.signed(self.image('hub')))
        forged[192 + 320:192 + 352] = image[320:352]
        self.assertEqual(self.upload(bytes(forged)), 400)
        mutated = bytearray(self.signed(image)); mutated[-1] ^= 1
        self.assertEqual(self.upload(bytes(mutated)), 400)

    def test_truncation_size_and_transport_failures(self):
        image = self.image('full')
        for unsigned in (False, True):
            data = image if unsigned else self.signed(image)
            for n in (0, 1, 191, 288, 319, 320, 351):
                self.assertEqual(self.upload(data[:n], unsigned=unsigned), 400)
            for n in range(320, 352):
                # Even a trusted signature cannot make an incomplete identity
                # acceptable. Check every truncation within the variant field.
                partial = image[:n]
                self.assertEqual(self.upload(partial if unsigned else self.signed(partial), unsigned=unsigned), 400)
            self.assertEqual(self.upload(data[:-1], unsigned=unsigned, declared=len(data)), 400)
            self.assertEqual(self.upload(data, unsigned=unsigned, declared=9000), 400)
            for fault in (1, 2, 3, 4, 5, 6, 7):
                self.assertEqual(self.upload(data, unsigned=unsigned, fault=fault), 400)

    def test_wrong_target_project_descriptor_and_unrelated_label(self):
        for offset in (0, 32, 80, 288):
            image = bytearray(self.image('full')); image[offset] ^= 1
            for unsigned in (False, True):
                self.assertEqual(self.upload(bytes(image) if unsigned else self.signed(image), unsigned=unsigned), 400)
        image = bytearray(self.image('full'))
        image[400:432] = b'PHICOMM-DAIKIN-1:hub'.ljust(32, b'\0')
        self.assertEqual(self.upload(self.signed(image)), 202)

    def test_unauthorized_upload_does_not_touch_flash(self):
        self.upload(self.signed(self.image('full')), allow=False)
        self.assertEqual(self.lib.count(0), 0)
        self.assertEqual(self.lib.count(1), 0)

    def test_conversion_success_uses_signed_hub_payload_and_exact_flash_readback(self):
        image = self.image('hub')
        signed = self.signed(image)
        for chunk in (1, 7, 191, 192, 353, 544, 1024):
            self.assertEqual(self.convert(signed, chunk=chunk), 202)
            self.assertEqual(self.lib.flash_matches(image, len(image)), 1)
            self.assertEqual(self.lib.count(0), 1)
            self.assertEqual(self.lib.count(10), 544, 'Flash erased before bounded prefix was received')
            self.assertGreater(self.lib.count(2), 1)
            self.assertEqual(self.lib.count(3), 1)
            self.assertEqual(self.lib.count(4), 1)
            self.assertEqual(self.lib.count(8), 1)
            self.assertEqual(self.lib.count(9), 1)
            self.assertEqual(json.loads(self.lib.reply())['code'], 'accepted')
            self.assertEqual(self.lib.response_header(b'Cache-Control'), b'no-store')

    def test_conversion_requires_valid_session_even_if_password_unset(self):
        signed = self.signed(self.image('hub'))
        for auth in (0, 2, 3, 4):
            self.assertEqual(self.convert(signed, auth=auth), 401)
            self.assertEqual(json.loads(self.lib.reply())['code'], 'unauthorized')
            self.assertEqual(self.lib.count(0), 0)
            self.assertEqual(self.lib.response_header(b'WWW-Authenticate'), b'')

    def test_conversion_headers_are_required_and_exact(self):
        signed = self.signed(self.image('hub'))
        for target, confirm in ((None, 'full-to-hub'), ('', 'full-to-hub'), ('full', 'full-to-hub'),
                                ('Hub', 'full-to-hub'), ('hub ', 'full-to-hub'), ('h' * 100, 'full-to-hub'),
                                ('hub', None), ('hub', ''), ('hub', 'hub-to-full'),
                                ('hub', 'Full-to-hub'), ('hub', 'full-to-hub '), ('hub', 'x' * 100)):
            self.assertEqual(self.convert(signed, target=target, confirm=confirm), 400)
            self.assertEqual(json.loads(self.lib.reply())['code'], 'invalid_params')
            self.assertEqual(self.lib.count(0), 0)

    def test_conversion_requires_valid_config_and_no_pending_operation(self):
        signed = self.signed(self.image('hub'))
        for busy in range(1, 5):
            self.assertEqual(self.convert(signed, busy=busy), 409)
            self.assertEqual(json.loads(self.lib.reply())['code'], 'busy')
            self.assertEqual(self.lib.count(0), 0)
        self.assertEqual(self.convert(signed, recovery=True), 401)
        self.assertEqual(json.loads(self.lib.reply())['code'], 'unauthorized')
        self.assertEqual(self.lib.count(0), 0)

    def test_conversion_cannot_run_from_hub_or_convert_back_to_full(self):
        self.assertEqual(self.convert(self.signed(self.image('full')), variant='hub', target='full',
                                      confirm='hub-to-full'), 400)
        self.assertEqual(self.convert(self.signed(self.image('hub')), variant='hub'), 409)
        self.assertEqual(json.loads(self.lib.reply())['code'], 'unsupported_conversion')
        self.assertEqual(self.lib.count(0), 0)

    def test_conversion_rejects_bad_fixed_identity_before_flash_erase(self):
        for variant in ('full', 'hub'):
            for offset in (0, 32, 80, 288, 320, 351):
                image = bytearray(self.image(variant)); image[offset] ^= 1
                image[400:432] = self.image('hub')[320:352]
                self.assertEqual(self.convert(self.signed(bytes(image))), 400)
                self.assertEqual(self.lib.count(0), 0)
        self.assertEqual(self.convert(self.signed(self.image('full'))), 400)
        self.assertEqual(self.lib.count(0), 0)

    def test_conversion_rejects_unsigned_untrusted_and_tampered_payloads(self):
        image = self.image('hub')
        invalid = bytearray(self.signed(image)); invalid[128:192] = bytes(64)
        for data in (image, bytes(invalid), self.signed(image, ec.generate_private_key(ec.SECP256R1()))):
            self.assertEqual(self.convert(data), 400)
            self.assertEqual(self.lib.count(0), 0)
        # A signed full header plus a forged hub label is still a digest mismatch.
        forged = bytearray(self.signed(self.image('full')))
        forged[192 + 320:192 + 352] = image[320:352]
        self.assertEqual(self.convert(bytes(forged)), 400)
        self.assertEqual(self.lib.count(0), 1)
        changed = bytearray(self.signed(image)); changed[-1] ^= 1
        self.assertEqual(self.convert(bytes(changed)), 400)

    def test_conversion_rejects_truncation_and_flash_failures_without_activation(self):
        image = self.image('hub'); signed = self.signed(image)
        for n in (0, 1, 191, 192, 319, 320, 543, 544):
            self.assertEqual(self.convert(signed[:n]), 400)
            self.assertEqual(self.lib.count(0), 0)
        for n in range(320, 352):
            self.assertEqual(self.convert(self.signed(image[:n])), 400)
            self.assertEqual(self.lib.count(0), 0)
        self.assertEqual(self.convert(signed[:-1], declared=len(signed)), 400)
        for fault in range(1, 9):
            self.assertEqual(self.convert(signed, fault=fault), 400)
            self.assertEqual(json.loads(self.lib.reply())['code'], 'invalid_image')

    def test_conversion_capacity_uses_actual_slot_and_excludes_signature_envelope(self):
        capacity = 0x180000
        image = bytearray(self.image('hub'))
        image.extend(bytes(capacity - len(image)))
        struct.pack_into('<I', image, 28, len(image) - 32)
        signed = self.signed(bytes(image))
        self.assertEqual(self.convert(signed, capacity=capacity), 202)
        self.assertEqual(self.lib.flash_matches(bytes(image), len(image)), 1)
        self.assertEqual(self.convert(signed, capacity=capacity - 1), 400)
        self.assertEqual(self.lib.count(0), 0)
        self.assertEqual(self.convert(signed, capacity=capacity, declared=len(signed) + 1), 400)
        self.assertEqual(self.lib.count(0), 0)

    def test_conversion_preflight_allows_platform_headers(self):
        self.lib.preflight()
        self.assertEqual(self.lib.response_header(b'Access-Control-Allow-Origin'), b'*')
        allowed = self.lib.response_header(b'Access-Control-Allow-Headers')
        for name in (b'Content-Type', b'Authorization', b'X-IoTCloud-Target-Variant', b'X-IoTCloud-Confirm'):
            self.assertIn(name, allowed)

    def test_large_uploads_erase_incrementally_and_yield_during_write_and_readback(self):
        size = 1479664  # The payload size that exposed the device's early reset.
        for variant, unsigned, conversion in (('full', False, False), ('full', True, False),
                                               ('hub', False, False), ('hub', True, False),
                                               ('hub', False, True)):
            image = bytearray(self.image(variant))
            image.extend((bytes(range(256)) * ((size - len(image) + 255) // 256))[:size - len(image)])
            struct.pack_into('<I', image, 28, len(image) - 32)
            payload = bytes(image)
            data = payload if unsigned else self.signed(payload)
            for chunk in (997, 8192):
                with self.subTest(variant=variant, unsigned=unsigned, conversion=conversion, chunk=chunk):
                    if conversion:
                        status = self.convert(data, chunk=chunk, capacity=0x180000)
                    else:
                        status = self.upload(data, variant, unsigned, chunk, capacity=0x180000)
                    self.assertEqual(status, 202)
                    self.assertEqual(self.lib.flash_matches(payload, len(payload)), 1)
                    self.assertEqual(self.lib.count(11), 1, 'Whole-image erase was requested')
                    self.assertEqual(self.lib.count(14), 4096, 'Erase exceeded one newly touched sector')
                    self.assertEqual(self.lib.count(16), (size + 4095) // 4096 * 4096)
                    self.assertGreater(self.lib.count(12), 0, 'Flash work never allowed other tasks to run')
                    self.assertLess(self.lib.count(13), 32768, 'Unbounded work between scheduler yields')
                    if conversion:
                        for fault in (9, 10):
                            self.assertEqual(self.convert(data, fault=fault, capacity=0x180000), 400)
                            self.assertEqual(self.lib.count(4), 0)
                            self.assertGreaterEqual(self.lib.count(5), 1, 'Failed sector erase did not abort OTA')


class PackageIdentity(unittest.TestCase):
    def package(self, image, variant, accepted):
        with tempfile.TemporaryDirectory(prefix='daikin-package-identity-') as tmp:
            root = Path(tmp)
            project = root / 'DAIKIN'
            project.mkdir()
            shutil.copyfile(ROOT / 'DAIKIN/package.py', project / 'package.py')
            release = root / 'tools/release'
            release.mkdir(parents=True)
            shutil.copyfile(ROOT / 'tools/release/products.py', release / 'products.py')
            name = 'bry88ab-hub' if variant == 'hub' else 'bry88ab'
            build = project / '.pio/build' / name
            build.mkdir(parents=True)
            (build / 'firmware.bin').write_bytes(image)
            generated = project / 'generated' / name
            generated.mkdir(parents=True)
            (generated / (name + '-version.json')).write_text('{"version":"1.0.0"}')
            (generated / 'frontend.json').write_text('{}')
            core = root / 'pio-core'
            tool = core / 'packages/tool-esptoolpy/esptool.py'
            tool.parent.mkdir(parents=True)
            tool.write_text('import sys\nfrom pathlib import Path\n'
                            'Path(sys.argv[sys.argv.index("-o")+1]).write_bytes(b"factory")\n')
            result = subprocess.run([sys.executable, str(project / 'package.py')],
                                    env={**os.environ, 'PHICOMM_FIRMWARE_VARIANT': variant,
                                         'PLATFORMIO_CORE_DIR': str(core)}, capture_output=True, text=True)
            out = root / ('build/daikin-release' if variant == 'full' else 'build/daikin-hub-release')
            if accepted:
                self.assertEqual(result.returncode, 0, result.stderr)
                manifest = json.loads((out / 'manifest.json').read_text())
                self.assertEqual(manifest['firmware_variant'], variant)
                self.assertEqual(manifest['variants']['auto']['firmware_variant'], variant)
                self.assertEqual((out / manifest['variants']['auto']['application']).read_bytes(), image)
            else:
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('identity does not match package target', result.stderr)
                self.assertFalse(list(out.glob('*.bin')))
                self.assertFalse((out / 'manifest.json').exists())

    def test_both_package_targets_match_actual_image_identity(self):
        for variant in ('full', 'hub'):
            self.package(OtaIdentity.image(variant), variant, True)

    def test_wrong_variant_and_legacy_image_cannot_be_packaged_as_new_release(self):
        for source, target in (('hub', 'full'), ('full', 'hub')):
            self.package(OtaIdentity.image(source), target, False)
        image = bytearray(OtaIdentity.image('full'))
        image[320:352] = bytes(32)
        self.package(bytes(image), 'full', False)


class VariantHttpSurface(unittest.TestCase):
    def test_compiled_full_exposes_conversion_and_hub_does_not(self):
        # Compile the real capability function and setup registration block
        # under both build defines; a runtime fixture alone cannot establish
        # that the hub binary has no conversion route.
        commands = (ROOT / 'DAIKIN/src/commands.inc').read_text()
        capabilities = 'static const char *actions[]=' + commands.split(
            'static const char *actions[]=', 1)[1].split('static bool knownFields(', 1)[0]
        main = (ROOT / 'DAIKIN/src/main.cpp').read_text()
        registration = '#ifndef IOTCLOUD_HUB\n    httpd_uri_t convert=' + main.split(
            '#ifndef IOTCLOUD_HUB\n    httpd_uri_t convert=', 1)[1].split('#endif', 1)[0] + '#endif\n'
        includes = [ROOT / 'DAIKIN/.pio/libdeps/bry88ab-hub/ArduinoJson/src',
                    ROOT / 'DAIKIN/.pio/libdeps/bry88ab/ArduinoJson/src']
        codec = next((path for path in includes if (path / 'ArduinoJson.h').exists()), None)
        if codec is None:
            raise RuntimeError('Build one DAIKIN PlatformIO target to fetch the pinned ArduinoJson dependency')
        with tempfile.TemporaryDirectory(prefix='daikin-ota-surface-') as temporary:
            directory = Path(temporary)
            (directory / 'capabilities.inc').write_text(capabilities)
            (directory / 'registration.inc').write_text(registration)
            (directory / 'surface.cpp').write_text(r'''
#include <ArduinoJson.h>
#include <iostream>
#include <string>
#include <vector>
static const char *deviceId="9c9c1ff76748",*version="1.0.0-rc.4";
static bool configRecovery=false,hchoPresent=false;
#include "capabilities.inc"
static std::vector<std::string> routes;
#ifndef IOTCLOUD_HUB
enum {HTTP_POST,HTTP_OPTIONS};
struct httpd_uri_t {const char *uri;int method;int (*handler)(void *);};
static int convertHandler(void *){return 0;}
static int optionsHandler(void *){return 0;}
static const int web=1;
static void httpd_register_uri_handler(int,const httpd_uri_t *uri){
  routes.push_back(std::string(uri->method==HTTP_POST?"POST ":"OPTIONS ")+uri->uri);
}
#endif
int main(){
  #include "registration.inc"
  DynamicJsonDocument result(2048);
  capabilities(result.createNestedObject("capabilities"));
  JsonArray registered=result.createNestedArray("routes");
  for(const auto &route:routes)registered.add(route);
  serializeJson(result,std::cout);
}
''')
            for variant in ('full', 'hub'):
                with self.subTest(variant=variant):
                    binary = directory / variant
                    defines = ['-DDAIKIN_VARIANT="' + variant + '"']
                    if variant == 'hub':
                        defines.append('-DIOTCLOUD_HUB')
                    subprocess.run(['c++', '-std=c++11', '-Wall', '-Wextra', '-Werror',
                                    '-I' + str(codec), *defines, str(directory / 'surface.cpp'),
                                    '-o', str(binary)], check=True)
                    result = json.loads(subprocess.check_output([str(binary)], text=True))
                    for key,value in {'device_id':'9c9c1ff76748','model':'daikin','firmware_version':'1.0.0-rc.4','variant':variant}.items():
                        self.assertEqual(result['capabilities'][key],value)
                    self.assertEqual(result['capabilities']['sensor_session_protocol'],3)
                    if variant == 'full':
                        self.assertEqual(result['capabilities']['ota_variant_conversion'], 'full-to-hub')
                        self.assertEqual(result['routes'], ['POST /api/v1/ota/convert',
                                                            'OPTIONS /api/v1/ota/convert'])
                    else:
                        self.assertNotIn('ota_variant_conversion', result['capabilities'])
                        self.assertEqual(result['routes'], [])


if __name__ == '__main__':
    unittest.main()
