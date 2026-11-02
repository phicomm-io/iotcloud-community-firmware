#!/usr/bin/env python3
"""Pinned OpenBeken + BK7231N SDK build. Compilers/runtimes live in shared cache."""
import argparse
import difflib
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import zlib
from verify_firmware import crc16,crc_blocks
from source_provenance import verify_sources
from check_stack import verify_stack

HERE = Path(__file__).resolve().parent
CACHE = Path(os.environ.get('PHICOMM_TOOLS_HOME', Path.home()/'.cache/phicomm'))
APP_SHA = '7b754d6fc80b72115ece31fcf4527f011f90deef'
SDK_SHA = '07fd9e0e2da0aabaddab00be2a897589be235158'
APP_CACHE = CACHE/'openbeken/app-1.18.319'
SDK_CACHE = CACHE/'openbeken/bk7231n-upstream'
APP_NAME = 'OpenBK7231N'
SDK_MDNS_FILES = (
    'platforms/bk7231n/bk7231n_os/beken378/func/lwip_intf/lwip-2.1.3/src/apps/mdns/mdns.c',
    'platforms/bk7231n/bk7231n_os/beken378/func/lwip_intf/lwip-2.1.3/src/include/lwip/apps/mdns.h',
)
COPY_LOCAL_IP_FUNCTION = '''#ifndef PH_HOST_TEST
#include "lwip/ip4_addr.h"
#endif
int PH_BL_CopyLocalIPAddress(char *out, size_t capacity)
{
    struct wlan_ip_config snapshot;
    void *interface;
    int buflen;

    if (!out || !capacity) return 0;
    out[0] = '\\0';
    buflen = capacity < IP4ADDR_STRLEN_MAX ? (int)capacity : IP4ADDR_STRLEN_MAX;
    interface = g_bOpenAccessPointMode ? net_get_uap_handle() : net_get_sta_handle();
    if (!interface) return 0;
    memset(&snapshot, 0, sizeof(snapshot));
    if (net_get_if_addr(&snapshot, interface) != 0 || snapshot.ipv4.address == 0)
        return 0;
    if (!ip4addr_ntoa_r((const ip4_addr_t *)&snapshot.ipv4.address, out, buflen)) {
        out[0] = '\\0';
        return 0;
    }
    return 1;
}
'''
sys.path.insert(0,str(HERE.parent/'tools'))
from check_version import resolve_version

def command(args, cwd=None, log=None):
    if log:
        log.write(('\n$ '+' '.join(map(str,args))+'\n').encode()); log.flush()
    subprocess.run(list(map(str,args)), cwd=cwd, stdout=log, stderr=subprocess.STDOUT if log else None, check=True)

def pinned_sources():
    APP_CACHE.parent.mkdir(parents=True, exist_ok=True)
    if not APP_CACHE.exists():
        command(['git','clone','--depth=1','--branch','1.18.319','https://github.com/openshwprojects/OpenBK7231T_App.git',APP_CACHE])
    if not SDK_CACHE.exists():
        command(['git','clone','--filter=blob:none','--no-checkout','https://github.com/openshwprojects/OpenBK7231N.git',SDK_CACHE])
        command(['git','sparse-checkout','set','--no-cone','/*','!/platforms/bk7231n/toolchain/gcc-arm-none-eabi-4_9-2015q1/','!/platforms/bk7231n/toolchain/windows/'],SDK_CACHE)
        command(['git','checkout',SDK_SHA],SDK_CACHE)
    for path, sha in [(APP_CACHE,APP_SHA),(SDK_CACHE,SDK_SHA)]:
        actual=subprocess.check_output(['git','rev-parse','HEAD'],cwd=path,text=True).strip()
        if actual != sha:
            raise SystemExit(f'Unexpected shared source HEAD in {path}: {actual}, expected {sha}')
        if subprocess.check_output(['git','status','--porcelain','--untracked-files=no'],cwd=path,text=True):
            raise SystemExit(f'Shared upstream is modified: {path}')

def overlay(app, sdk, run, boot_trace=False, stock_layout=True):
    patches=[]
    def replace(relative, before, after):
        p=app/relative; old=p.read_text()
        if old.count(before)!=1: raise SystemExit(f'Framework patch no longer matches: {relative}: {before[:60]}')
        new=old.replace(before,after);p.write_text(new)
        patches.extend(difflib.unified_diff(old.splitlines(True),new.splitlines(True),fromfile='a/'+relative,tofile='b/'+relative))
    replace('src/user_main.c','#include "new_common.h"','#include "new_common.h"\n#include "phicomm/src/port.h"\n#include "phicomm/src/boot_trace.h"\n#include "phicomm/src/memory_policy.h"')
    replace('src/user_main.c','\tMain_Init_Before_Delay();','\tPH_BOOT_STAGE("before-delay");\n\tMain_Init_Before_Delay();\n\tPH_BOOT_STAGE("before-delay-done");')
    replace('src/user_main.c','\tMain_Init_Delay();','\tPH_BOOT_STAGE("calibration-wait");\n\tMain_Init_Delay();\n\tPH_BOOT_STAGE("calibration-done");')
    replace('src/user_main.c','\tMain_Init_After_Delay();','\tPH_BOOT_STAGE("after-delay");\n\tMain_Init_After_Delay();\n\tPH_BOOT_STAGE("after-delay-done");')
    replace('src/hal/bk7231/hal_main_bk7231.c','#include "../../new_common.h"','#include "../../new_common.h"\n#include "../../phicomm/src/boot_trace.h"')
    replace('src/hal/bk7231/hal_main_bk7231.c','\tMain_Init();','\tPH_BOOT_START();\n\tMain_Init();\n\tPH_BOOT_STAGE("main-returned");')
    replace('src/hal/bk7231/hal_main_bk7231.c','\tADDLOGF_DEBUG("initialised oneshot timers");','\tADDLOGF_DEBUG("initialised oneshot timers");\n\tPH_BOOT_STAGE("timers-ready");')
    # SDK printf and OpenBeken's buffered logger have independent port selectors.
    # UART2/P1 belongs exclusively to the 4800 8E1 HLW8032 meter. Set the native
    # logger's defaults before any timer callback can emit onto that UART.
    replace('src/logging/logging.c',
        'int UART_PORT = UART2_PORT;\nint UART_PORT_INDEX = 1;',
        'int UART_PORT = UART1_PORT;\nint UART_PORT_INDEX = 0;')
    replace('src/user_main.c','void QuickTick(void* param)\n{','void QuickTick(void* param)\n{\n    PH_BL_Tick();')
    replace('src/user_main.c','void Main_OnEverySecond()\n{','void Main_OnEverySecond()\n{\n    PH_BOOT_STAGE("second-enter");')
    replace('src/user_main.c','\tHAL_Run_WDT();\n#endif','\tHAL_Run_WDT();\n#endif\n    PH_BOOT_STAGE("second-done");')
    # The three dedicated service stacks reserve 44 KiB. The generic 25 kB
    # floor reboots this port after six healthy checks, before Wi-Fi settles.
    # OTA closes MQTT/UDP and uses small streaming buffers. It retains the
    # same sustained low-heap recovery floor as normal operation.
    replace('src/user_main.c','\tif (xPortGetFreeHeapSize() < 25 * 1000) {',
        '\tif (xPortGetFreeHeapSize() < PH_SERVICE_MEMORY_RESERVE) {')
    replace('src/user_main.c','\t\tg_secondsSpentInLowMemoryWarning++;',
        '\t\tg_secondsSpentInLowMemoryWarning++;\n'
        '\t\tPH_BOOT_RESULT("heap-warning", g_secondsSpentInLowMemoryWarning);')
    replace('src/user_main.c','\t\tif (g_secondsSpentInLowMemoryWarning > 5) {\n\t\t\tHAL_RebootModule();',
        '\t\tif (g_secondsSpentInLowMemoryWarning > 5) {\n'
        '\t\t\tPH_BOOT_STAGE("heap-reboot");\n\t\t\tHAL_RebootModule();')
    replace('src/user_main.c','\tPIN_ticks(param);','\t/* Dedicated socket GPIO is handled by PH_BL_Tick. */')
    replace('src/user_main.c','\tPIN_SetupPins();','\t/* Product GPIO is initialized before the shared services. */')
    replace('src/user_main.c','\tQuickTick_StartThread();','\t/* Product ticks also run in native safe mode. */')
    replace('src/user_main.c','HAL_SetupWiFiOpenAccessPoint(CFG_GetDeviceName());','if (PH_BL_APName()[0]) HAL_SetupWiFiOpenAccessPoint(PH_BL_APName());')
    # AP client association is not a station connection to a router. Keeping
    # these states separate also prevents an AP phone from suppressing retries
    # or hiding the product's Wi-Fi failure indicator.
    replace('src/user_main.c','case WIFI_AP_CONNECTED:\n\t\tg_bHasWiFiConnected = 1;',
        'case WIFI_AP_CONNECTED:\n\t\t/* AP client only; retain STA connection state. */')
    replace('src/user_main.c','case WIFI_AP_FAILED:\n\t\tg_bHasWiFiConnected = 0;',
        'case WIFI_AP_FAILED:\n\t\t/* AP client only; retain STA connection state. */')
    replace('src/hal/bk7231/hal_wifi_bk7231.c','"192.168.4.1"\n#define APP_DRONE_DEF_NET_MASK','"192.168.0.1"\n#define APP_DRONE_DEF_NET_MASK')
    replace('src/hal/bk7231/hal_wifi_bk7231.c','"192.168.4.1"\n#define APP_DRONE_DEF_CHANNEL','"192.168.0.1"\n#define APP_DRONE_DEF_CHANNEL')
    replace('src/hal/bk7231/hal_wifi_bk7231.c','const char* HAL_GetMyIPString() {',
        COPY_LOCAL_IP_FUNCTION+'\nconst char* HAL_GetMyIPString() {')
    replace('src/user_main.c','\tCFG_ApplyChannelStartValues();','\t/* No generic channel/GPIO ownership in this product. */')
    replace('src/user_main.c','\tinit_rest();','\t/* Legacy JSON REST is replaced by PH_BL_HTTP. */')
    replace('src/cmnds/cmd_tasmota.c','\tCMD_RegisterCommand("Json", cmnd_JsonCommand, NULL);','\t/* JSON command compatibility is disabled; JSON is HA-only. */')
    # The native HTTP start precedes AfterDelay_Unsafe, which is skipped in
    # safe mode. A not-ready product closes shared HTTP admission; initialize
    # it and its button/meter timer first on both normal and recovery boots.
    replace('src/user_main.c','\tHTTPServer_Start();',
        '\tPH_BL_Init(bSafeMode);\n\tPH_BOOT_STAGE("product-returned");\n\tQuickTick_StartThread();\n\tPH_BOOT_STAGE("quick-timer-ready");\n\tPH_BL_StartHTTP();')
    # Native HTTP client has a whole-transfer timeout. Flash writes on this
    # board exceed its upstream 10 seconds; failed transfers still emit state 2.
    ota='src/hal/bk7231/hal_ota_bk7231.c'
    replace(ota,'#include "../hal_ota.h"','#include "../hal_ota.h"\n#include "../../phicomm/src/ota_guard.h"\n#include "../../littlefs/our_lfs.h"')
    replace(ota,'httprequest_t httprequest;',
        'httprequest_t httprequest;\nstatic ph_ota_guard ph_ota;\n'
        'static char *http_buf;\n'
        'static void ph_ota_release_response(httprequest_t *request) {\n'
        '  os_free(http_buf); http_buf = NULL;\n'
        '  request->client_data.response_buf = NULL;\n'
        '  request->client_data.response_buf_len = 0;\n'
        '}')
    replace(ota,'char *http_buf = (void *)0;','/* Response buffer is owned and freed by this OTA request. */')
    replace(ota,'  OTA_SetTotalBytes(OTA_GetTotalBytes() + request->client_data.response_buf_filled);',
        '  /* The public progress denominator is the complete HTTP body. */\n'
        '  if (request->state == 1) OTA_SetTotalBytes(request->client_data.response_content_len);')
    replace(ota,'    case 0: // start','    case -1: case -2:\n      ph_ota.failed = true;\n      break;\n    case 0: // start\n      memset(&ph_ota, 0, sizeof(ph_ota));')
    replace(ota,'      init_ota(START_ADR_OF_BK_PARTITION_OTA);','      if (!init_ota(START_ADR_OF_BK_PARTITION_OTA)) ph_ota.failed = true;')
    replace(ota,'        sector = os_malloc(SECTOR_SIZE);','        sector = os_malloc(SECTOR_SIZE);\n        if (!sector) return 0;')
    # A 4 KiB erase unit does not require a 4 KiB RAM buffer. Stream 512-byte
    # writes, erasing only at physical sector boundaries; retain RBL bytes.
    replace(ota,'#define SECTOR_SIZE 0x1000','#define SECTOR_SIZE 512')
    replace(ota,'    flash_ctrl(CMD_FLASH_WRITE_ENABLE, (void *)0);\n    flash_ctrl(CMD_FLASH_ERASE_SECTOR, &addr);',
        '    if (!(addr & 4095u)) {\n'
        '      flash_ctrl(CMD_FLASH_WRITE_ENABLE, (void *)0);\n'
        '      flash_ctrl(CMD_FLASH_ERASE_SECTOR, &addr);\n    }')
    replace(ota,'        addr += 1024;','        addr += SECTOR_SIZE;')
    replace(ota,'#define BUF_SIZE 2048','#define BUF_SIZE 512')
    replace(ota,'    case 1: // data','    case 1: { // data\n      unsigned int lfs_size = CFG_GetLFS_Size();\n      unsigned int room = LFS_BLOCKS_END - START_ADR_OF_BK_PARTITION_OTA;\n      unsigned int limit = lfs_size < room ? (room - lfs_size) & ~4095u : 0;\n      if (!ph_ota_chunk(&ph_ota, request->client.response_code, client_data->response_content_len,\n          (unsigned char*)client_data->response_buf, client_data->response_buf_filled, limit)) return 1;')
    replace(ota,'    case 2: // ended, write any remaining bytes to the sector','    }\n    case 2: // ended, write any remaining bytes to the sector\n      if (!ph_ota_complete(&ph_ota)) {\n        if (sector) {\n          unsigned int start = START_ADR_OF_BK_PARTITION_OTA;\n          os_free(sector); sector = NULL; sectorlen = 0;\n          flash_ctrl(CMD_FLASH_WRITE_ENABLE, NULL);\n          flash_ctrl(CMD_FLASH_ERASE_SECTOR, &start);\n        }\n        flash_protection_op(FLASH_XTX_16M_SR_WRITE_ENABLE, FLASH_UNPROTECT_LAST_BLOCK);\n        ph_ota_release_response(request);\n        OTA_ResetProgress();\n        addLogAdv(LOG_ERROR, LOG_FEATURE_OTA, "Incomplete OTA rejected; staying online");\n        return 0;\n      }')
    replace(ota,'      close_ota();\n      OTA_ResetProgress();',
        '      close_ota();\n      ph_ota_release_response(request);\n'
        '      /* Remain reserved while saving configuration and rebooting. */')
    # Publish active progress before the new thread can finish. The native
    # request owns this buffer (no FREE_RESPONSEBUF flag); free it on errors
    # and thread-creation failure rather than retaining 2 KiB after a retry.
    replace(ota,'  HTTPClient_Async_SendGeneric(request);\n  //+2 Updating ota_status to 0 as before.\n  OTA_ResetProgress();\n  OTA_IncrementProgress(1);',
        '  OTA_ResetProgress();\n  OTA_IncrementProgress(1);\n'
        '  if (HTTPClient_Async_SendGeneric(request) != 0) {\n'
        '    ph_ota_release_response(request);\n    OTA_ResetProgress();\n  }')
    replace(ota,'  request->timeout = 10000;','  request->timeout = 120000;')
    # The upstream reader computes a timeout but passes NULL to select, then
    # loops forever when select times out or fails without setting the fd.
    # Enforce the native client's remaining whole-transfer deadline.
    net='src/httpclient/utils_net.c'
    replace(net,'        ret = select(fd + 1, &sets, NULL, NULL, NULL);',
        '        if (!t_left) break;\n'
        '        ret = select(fd + 1, &sets, NULL, NULL, &timeout);\n'
        '        if (ret == 0) break;\n'
        '        if (ret < 0) {\n'
        '            if (errno == EINTR) continue;\n'
        '            err_code = -2; break;\n'
        '        }')
    # A zero-byte deadline is not a successful empty response. Propagate it
    # through header parsing as well as body reads so the native worker emits
    # error -> end and releases the OTA reservation instead of spinning.
    http='src/httpclient/http_client.c'
    # Per-255-byte logging is unnecessary for the public OTA progress counter.
    # Keep it available at debug level without pacing normal downloads on UART.
    replace(http,'    ADDLOG_INFO(LOG_FEATURE_HTTP_CLIENT, "httpclient_recv %u bytes has been read", *p_read_len);',
        '    ADDLOG_DEBUG(LOG_FEATURE_HTTP_CLIENT, "httpclient_recv %u bytes has been read", *p_read_len);')
    replace(ota,'      addLogAdv(LOG_INFO, LOG_FEATURE_OTA,"%x", addr);',
        '      addLogAdv(LOG_DEBUG, LOG_FEATURE_OTA,"%x", addr);')
    replace(http,'        //timeout\n        return 0;',
        '        /* The read deadline expired without data. */\n'
        '        return ERROR_NET_TIMEOUT;')
    replace(http,'                if (ret == ERROR_HTTP_CONN) {\n'
        '                    return ret;\n'
        '                } else {\n'
        '                    continue;\n'
        '                }\n'
        '            } else {\n'
        '                ADDLOG_DEBUG(LOG_FEATURE_HTTP_CLIENT, "header len > chunksize");',
        '                if (ret < 0) {\n'
        '                    return ret;\n'
        '                } else {\n'
        '                    continue;\n'
        '                }\n'
        '            } else {\n'
        '                ADDLOG_DEBUG(LOG_FEATURE_HTTP_CLIENT, "header len > chunksize");')
    timer='src/httpclient/utils_timer.c'
    replace(timer,'    return hal_machw_time()/1000;','    return rtos_get_time();')
    replace(timer,'\tint t = hal_machw_time();\n\treturn (t < (int)(t_end * 1000)) ? 1 : 0;',
        '\tuint32_t left = (uint32_t)t_end - (uint32_t)t_now;\n'
        '\treturn (int32_t)left > 0 ? left : 0;')
    # This board's factory RF/TLV sector is at 0x1e0000, outside LFS/OTA.
    relative='platforms/bk7231n/bk7231n_os/beken378/func/user_driver/BkDriverFlash.c'
    p=sdk/relative;old=p.read_text();start=old.index('[BK_PARTITION_RF_FIRMWARE]');stop=old.index('[BK_PARTITION_NET_PARAM]',start)
    part=old[start:stop]
    if part.count('0x1D0000')!=1:raise SystemExit('RF partition SDK patch no longer matches')
    new=old[:start]+part.replace('0x1D0000','0x1E0000')+old[stop:];p.write_text(new)
    (run/'sdk-board.patch').write_text(''.join(difflib.unified_diff(old.splitlines(True),new.splitlines(True),fromfile='a/'+relative,tofile='b/'+relative)))
    # The pinned IRDA driver disables the RNG after every read, while its
    # initialization deliberately leaves the enable write commented out.
    # On this board the first sample then repeats after reboot. Keep the SDK
    # generator running from driver initialization through subsequent reads.
    relative='platforms/bk7231n/bk7231n_os/beken378/driver/irda/irda.c'
    p=sdk/relative;old=p.read_text()
    before='\t//REG_WRITE(TRNG_CTRL, value);'
    off='    reg = REG_READ(TRNG_CTRL);\n    reg &= ~TRNG_EN;\n    REG_WRITE(TRNG_CTRL, reg);'
    if old.count(before)!=1 or old.count(off)!=1:raise SystemExit('RNG SDK patch no longer matches')
    new=old.replace(before,'\tREG_WRITE(TRNG_CTRL, value);').replace(off,'    /* Socket firmware keeps the generator enabled between samples. */')
    p.write_text(new)
    with (run/'sdk-board.patch').open('a') as patch:
        patch.write(''.join(difflib.unified_diff(old.splitlines(True),new.splitlines(True),fromfile='a/'+relative,tofile='b/'+relative)))
    # Manual scans stream borrowed beacon frames into the product's fixed
    # metadata list. They never allocate or release the supplicant's full IE
    # table. Connection/reconnection scans retain the native SDK behavior.
    relative='platforms/bk7231n/bk7231n_os/beken378/func/rwnx_intf/rw_msg_rx.c'
    p=sdk/relative;old=p.read_text();new=old
    changes=[
        ('/* scan result malloc item */',
         'extern void PH_BL_ScanResult(const unsigned char *,unsigned,int);\n'
         'extern void PH_BL_ScanCompleted(void *, uint8_t);\n'
         'static void ph_scan_result_view(struct ke_msg *msg) {\n'
         '    SCAN_IND_PTR ind = (SCAN_IND_PTR)msg->param;\n'
         '    UINT32 prefix = (UINT8 *)ind->payload - (UINT8 *)ind;\n'
         '    if (msg->param_len < prefix || ind->length > msg->param_len - prefix) return;\n'
         '    PH_BL_ScanResult((const unsigned char *)ind->payload,ind->length,ind->rssi);\n'
         '}\n\n/* scan result malloc item */'),
        ('    return os_zalloc(vies_len + sizeof(struct sta_scan_res));',
         '    if (vies_len > (UINT32)-1 - sizeof(struct sta_scan_res)) return 0;\n'
         '    return os_zalloc(vies_len + sizeof(struct sta_scan_res));'),
        ('    ptr = os_zalloc(layer1_space_len + layer2_space_len);\n\n    ASSERT(ptr);',
         '    ptr = os_zalloc(layer1_space_len + layer2_space_len);'),
        ('        mhdr_scanu_start_cfm(rx_msg, scan_rst_set_ptr);',
         '        mhdr_scanu_start_cfm(rx_msg, scan_cfm_cb.cb == PH_BL_ScanCompleted ? 0 : scan_rst_set_ptr);'),
        ('    case SCANU_RESULT_IND:\n',
         '    case SCANU_RESULT_IND:\n'
         '        if (scan_cfm_cb.cb == PH_BL_ScanCompleted) {\n'
         '            ph_scan_result_view(rx_msg);\n            break;\n        }\n'),
        ('            ASSERT(scan_rst_set_ptr);',
         '            if (!scan_rst_set_ptr) break; /* Keep the radio task alive on OOM. */'),
    ]
    for before,after in changes:
        if new.count(before)!=1:raise SystemExit('Scan SDK patch no longer matches: '+before[:60])
        new=new.replace(before,after)
    p.write_text(new)
    with (run/'sdk-board.patch').open('a') as patch:
        patch.write(''.join(difflib.unified_diff(old.splitlines(True),new.splitlines(True),fromfile='a/'+relative,tofile='b/'+relative)))
    if boot_trace:
        relative='platforms/bk7231n/bk7231n_os/beken378/os/FreeRTOSv9.0.0/FreeRTOS/Source/portable/MemMang/heap_4.c'
        p=sdk/relative;old=p.read_text()
        before='\t\t\textern void vApplicationMallocFailedHook( void );'
        if old.count(before)!=1:raise SystemExit('Allocator diagnostic patch no longer matches')
        new=old.replace(before,'\t\t\tbk_printf("[S30-BOOT] malloc-failed wanted=%u free=%u\\r\\n", (unsigned)xWantedSize, (unsigned)xFreeBytesRemaining);\n'+before)
        p.write_text(new)
        with (run/'sdk-board.patch').open('a') as patch:
            patch.write(''.join(difflib.unified_diff(old.splitlines(True),new.splitlines(True),fromfile='a/'+relative,tofile='b/'+relative)))
    # The SDK responder reports allocation/bind failure and bounds TXT octets.
    # The incremental path restores these pinned inputs before each overlay.
    mdns_patch=HERE/'patches/mdns-init-checked.patch'
    with (run/'sdk-mdns-patch.log').open('wb') as patch_log:
        command(['patch','-p1','--batch','-i',mdns_patch],cwd=sdk,log=patch_log)
    with (run/'sdk-board.patch').open('a') as patch:
        patch.write(mdns_patch.read_text())
    # Password and grant changes share one authoritative atomic product record.
    replace('src/new_cfg.c','#include "new_cfg.h"','#include "new_cfg.h"\n#include "phicomm/src/port.h"')
    replace('src/new_cfg.c',
        'const char *CFG_GetOpenBekenHostName() {\n'
        '\tif (CFG_HasFlag(OBK_FLAG_USE_SHORT_DEVICE_NAME_AS_HOSTNAME)) {\n'
        '\t\treturn CFG_GetShortDeviceName();\n\t}\n'
        '\treturn CFG_GetDeviceName();\n}',
        'const char *CFG_GetOpenBekenHostName() {\n'
        '\treturn PH_BL_HostName();\n}')
    replace('src/new_cfg.c','\t// copy shortDeviceName to MQTT Client ID, set version=3',
        '\tunsigned char factory_mac[6];\n'
        '\tif (PH_BL_FactoryMac(factory_mac)) {\n'
        '\t\tCFG_SetMac((char*)factory_mac);\n'
        '\t\tWiFI_SetMacAddress((char*)factory_mac);\n'
        '\t}\n\n\t// copy shortDeviceName to MQTT Client ID, set version=3')
    replace('src/new_cfg.c','mainConfig_t g_cfg = { 0 };','mainConfig_t g_cfg = { 0 };\nint PH_BL_MqttTlsEnabled(void) { return g_cfg.mqtt_use_tls; }')
    # Product auth owns the atomic password+grant record. Native callers enter
    # that transaction before changing g_cfg; its raw adapter never recurses.
    replace('src/new_cfg.c','void CFG_SetWebPassword(const char *s) {',
        'void CFG_SetWebPassword(const char *s) {\n'
        '\t(void)PH_BL_WebPasswordSet(s);\n}\n'
        'void PH_BL_SetWebPasswordMemory(const char *s) {')
    # No Berry runtime or TLS dependencies are pulled into this dedicated port.
    replace('components.mk','include $(OBK_DIR)/libraries/berry.mk','# Berry disabled for BL2028N')
    replace('components.mk','CPPDEFINES += -DOBK_VARIANT=$(OBK_VARIANT)',
            'CPPDEFINES += -DOBK_VARIANT=$(OBK_VARIANT) -DPH_S30_LAN_V4')
    with (app/'components.mk').open('a') as definitions:
        definitions.write('\nINCLUDES += -I./beken378/func/wolfssl\n')
    # The SDK recursively enumerates src even when interpreter features are off.
    shutil.rmtree(app/'src/berry',ignore_errors=True)
    shutil.rmtree(app/'src/libraries/IRremoteESP8266',ignore_errors=True)
    shutil.rmtree(app/'src/libraries/rc-switch',ignore_errors=True)
    # Roomba's source lacks its driver feature guard and uses MQTT-only macros.
    (app/'src/driver/drv_roomba.c').unlink(missing_ok=True)
    (app/'src/obk_config.h').write_bytes(
        f'#define PH_BL_BOOT_TRACE {int(boot_trace)}\n'.encode()+(HERE/'openbeken-config.h').read_bytes())
    target=app/'src/phicomm'
    shutil.rmtree(target,ignore_errors=True)
    shutil.copytree(HERE/'src',target/'src',dirs_exist_ok=True)
    # Pure fixed-width business logic, shared with TC1; no foreign RTOS/runtime.
    shutil.copytree(HERE/'protocol',target/'protocol',dirs_exist_ok=True)
    shutil.copytree(HERE/'common',target/'common',dirs_exist_ok=True)
    # This SDK flattens object names into Debug/obj. Preserve both shared cores.
    for service in ('httpd','mqtt'):
        directory=target/'protocol'/service
        (directory/'core.c').rename(directory/f'ph_{service}_core.c')
    from render_web import render, header
    html=render()
    (target/'src/web.h').write_text(header(html))
    (run/'web.html').write_bytes(html)
    (run/'framework.patch').write_text(''.join(patches))
    if stock_layout:
        from stock_layout import apply
        apply(app, sdk, run)
    (app/'beken378').symlink_to(sdk/'platforms/bk7231n/bk7231n_os/beken378',target_is_directory=True)

def prepare(run, boot_trace=False, stock_layout=True):
    pinned_sources()
    build=run/'build';app=build/'app';sdk=build/'sdk'
    if build.exists():raise SystemExit(f'Build directory exists; use --incremental to refresh overlay: {build}')
    shutil.copytree(APP_CACHE,app,ignore=shutil.ignore_patterns('.git','sdk','output'))
    shutil.copytree(SDK_CACHE,sdk,ignore=shutil.ignore_patterns('.git','toolchain'))
    (sdk/'apps'/APP_NAME).symlink_to(app,target_is_directory=True)
    overlay(app,sdk,run,boot_trace,stock_layout)

def normalize_factory(data,version):
    """SDK packager leaves bytes after C-string terminators uninitialized.
    Normalize only footer strings, then regenerate header CRC32 and block CRC16.
    Boot/application payloads and their checksums stay byte-identical.
    """
    data=bytearray(data)
    for end,name,ver in [(0x11000,b'bootloader',b'1.00'),(len(data),b'app',version.encode())]:
        footer=bytearray(crc_blocks(data[end-102:end]))
        if footer[:4]!=b'RBL\0' or footer[12:28].split(b'\0',1)[0]!=name:
            raise ValueError('Unexpected SDK factory footer')
        footer[12:28]=name.ljust(16,b'\0');footer[28:52]=ver.ljust(24,b'\0');footer[52:76]=bytes(24)
        footer[92:96]=zlib.crc32(footer[:92]).to_bytes(4,'little')
        encoded=b''.join(footer[i:i+32]+crc16(footer[i:i+32]).to_bytes(2,'big') for i in range(0,96,32))
        data[end-102:end]=encoded
    return bytes(data)

def package(run, version, log, metadata, boot_trace=False, stock_layout=True):
    build=run/'build';app=build/'app';sdk=build/'sdk'
    src=app/'output'/version;dest=run/'firmware';dest.mkdir(exist_ok=True)
    base=f'{APP_NAME}_{version}';raw=src/(base+'.bin')
    generate=sdk/'platforms/bk7231n/bk7231n_os/tools/generate'
    tool=generate/'package_tool/linux'
    shutil.copy2(raw,generate/(base+'.bin'))
    # Native factory packaging expects one 16-byte padding unit to complete a
    # 32-byte flash block. Supply deterministic erased bytes before encryption.
    plain=raw.read_bytes();code_length=(len(plain)+15)//32*32+16
    code_name=base+'.code.bin'
    (generate/code_name).write_bytes(plain.ljust(code_length,b'\xff'))
    flavors=[] if stock_layout else [('BK7231N',['510fb093','a3cbeadc','5993a17e','c7adeb03'],'bk7231n_bootloader_enc.bin'),('BK7231M',['00000000']*4,'bk7231n_bootloader.bin')]
    if stock_layout:
        from stock_layout import LAYOUT
        if (len(plain)+31)//32*34 > LAYOUT['app_end']-LAYOUT['app_start']-102:
            raise SystemExit('Application exceeds the stock bootloader app partition')
    for flavor,keys,boot in flavors:
        command([tool/'encrypt',code_name,*keys,'10000'],generate,log)
        # The SDK encrypt executable reads beyond EOF and emits extra bytes.
        # Its word cipher preserves length; the packager adds block padding.
        # Keep only the compiled program, independently decrypted by verification.
        encrypted=generate/(base+'.code_enc.bin')
        data=encrypted.read_bytes()
        if len(plain)%4 or len(data)<code_length:raise ValueError('Invalid SDK cipher output length')
        encrypted.write_bytes(data[:code_length])
        command([sys.executable,'mpytools.py',boot,encrypted.name],generate,log)
        command([tool/'beken_packager','config.json'],generate,log)
        qio=normalize_factory((generate/'all_1.00.bin').read_bytes(),version)
        (dest/f'BL2028N_{flavor}_QIO_{version}.bin').write_bytes(qio)
        (dest/f'BL2028N_{flavor}_UA_{version}.bin').write_bytes(qio[0x11000:])
    command([tool/'rt_ota_packaging_tool_cli','-f',base+'.bin','-v',version,'-o',base+'.rbl','-p','app','-c','gzip','-s','aes','-k','0123456789ABCDEF0123456789ABCDEF','-i','0123456789ABCDEF'],generate,log)
    command([tool/'package',base+'.rbl',base+'_UG.bin',version],generate,log)
    shutil.copy2(generate/(base+'.rbl'),dest/f'BL2028N_OTA_{version}.rbl')
    if stock_layout:
        from stock_layout import application_image
        uart=application_image(plain,(generate/(base+'.rbl')).read_bytes()[:96])
        (dest/f'BL2028N_STOCK_UA_{version}.bin').write_bytes(uart)
    ug=generate/(base+'_UG.bin')
    limit=0xAE000 if stock_layout else 679936
    ota_package=generate/(base+'.rbl') if stock_layout else ug
    if ota_package.stat().st_size>limit:raise SystemExit('OTA exceeds the selected upgrade partition')
    shutil.copy2(ug,dest/f'BL2028N_UG_{version}.bin')
    shutil.copy2(raw,dest/f'BL2028N_app_{version}.bin')
    for suffix in ['.axf','.map']:
        shutil.copy2(src/(base+suffix),run/(base+suffix))
    shutil.copy2(sdk/'platforms/bk7231n/bk7231n_os/tuya.map',run/'linker.map')
    (dest/'SHA256SUMS').write_text(''.join(f'{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.name}\n' for p in sorted(dest.iterdir()) if p.suffix in ['.bin','.rbl']))
    inputs={str(p.relative_to(HERE)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(HERE.rglob('*')) if p.is_file() and '__pycache__' not in str(p)}
    project_head=subprocess.check_output(['git','rev-parse','HEAD'],cwd=HERE,text=True).strip()
    branch=subprocess.check_output(['git','branch','--show-current'],cwd=HERE,text=True).strip()
    dirty=bool(subprocess.check_output(['git','status','--porcelain'],cwd=HERE,text=True))
    manifest={'version':version,'version_metadata':metadata,'framework':'OpenBeken','openbeken_sha':APP_SHA,'sdk_sha':SDK_SHA,'project_head':project_head,'branch':branch,'dirty':dirty,'compiler':'GCC 5.4.1','boot_trace':boot_trace,'source_sha256':inputs,'hardware_verified':False}
    if stock_layout: manifest['flash_layout']=LAYOUT
    (run/'build-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run-dir',type=Path,required=True)
    parser.add_argument('--tag',help='Exact vMAJOR.MINOR.PATCH release tag, following tools/check_version.py')
    parser.add_argument('--jobs',type=int,default=min(os.cpu_count() or 2,8))
    parser.add_argument('--prepare-only',action='store_true')
    parser.add_argument('--incremental',action='store_true')
    parser.add_argument('--keep-build',action='store_true')
    parser.add_argument('--boot-trace',action='store_true',help='Diagnostic UART1 TX/P11 log at 4800 8E1; disable for final acceptance')
    parser.add_argument('--stock-layout',action='store_true',default=True,help='Default S30-V1B stock-bootloader layout (option retained for existing build scripts)')
    args=parser.parse_args();run=args.run_dir.resolve();run.mkdir(parents=True,exist_ok=True)
    verify_sources()
    tag=args.tag or (os.environ.get('GITHUB_REF_NAME') if os.environ.get('GITHUB_REF_TYPE')=='tag' else None)
    metadata=resolve_version(HERE.parent,tag=tag);version=metadata['version']
    if len(version)>31:parser.error('Repository version exceeds the SDK version field')
    (run/'version.json').write_text(json.dumps(metadata,indent=2)+'\n')
    gcc=CACHE/'gcc5/bin/arm-none-eabi-gcc'
    if subprocess.check_output([gcc,'-dumpversion'],text=True).strip()!='5.4.1':raise SystemExit('This checkout requires shared GCC 5.4.1')
    if not args.incremental:prepare(run,args.boot_trace,args.stock_layout)
    else:
        pinned_sources();app=run/'build/app';sdk=run/'build/sdk'
        (sdk/'apps'/APP_NAME).unlink(missing_ok=True)
        (sdk/'apps'/APP_NAME).symlink_to(app,target_is_directory=True)
        # Restore only files that this overlay owns before applying updated hooks.
        for rel in ['src/user_main.c','src/new_cfg.c','src/hal/bk7231/hal_main_bk7231.c','src/hal/bk7231/hal_wifi_bk7231.c','src/hal/bk7231/hal_ota_bk7231.c','src/httpclient/http_client.c','src/httpclient/utils_net.c','src/httpclient/utils_timer.c','src/logging/logging.c','src/cmnds/cmd_tasmota.c','src/httpserver/new_http.c','src/httpserver/http_tcp_server.c','src/httpserver/http_fns.c','components.mk']:
            shutil.copy2(APP_CACHE/rel,app/rel)
            if rel.endswith('.c'):
                (app/Path(rel).with_suffix('.o')).unlink(missing_ok=True)
        for relative in ['platforms/bk7231n/bk7231n_os/beken378/func/user_driver/BkDriverFlash.c',
                         'platforms/bk7231n/bk7231n_os/beken378/driver/irda/irda.c',
                         'platforms/bk7231n/bk7231n_os/beken378/func/rwnx_intf/rw_msg_rx.c',
                         'platforms/bk7231n/bk7231n_os/beken378/os/FreeRTOSv9.0.0/FreeRTOS/Source/portable/MemMang/heap_4.c',
                         *SDK_MDNS_FILES]:
            shutil.copy2(SDK_CACHE/relative,sdk/relative)
            (sdk/Path(relative).with_suffix('.o')).unlink(missing_ok=True)
        for rel in ['src/hal/hal_ota.h','src/littlefs/our_lfs.h']:
            shutil.copy2(APP_CACHE/rel,app/rel)
        (app/'beken378').unlink(missing_ok=True)
        overlay(app,sdk,run,args.boot_trace,args.stock_layout)
        # Overlay revisions may remove headers or whole translation units. Old
        # generated dependencies must not retain deleted implementation files.
        for dep in (sdk/'platforms/bk7231n/bk7231n_os/Debug/obj').glob('*.d'):
            dep.unlink()
    if args.prepare_only:return
    with (run/'compiler.log').open('wb') as log:
        command([gcc,'-v'],log=log)
    osdir=run/'build/sdk/platforms/bk7231n/bk7231n_os'
    with (run/'build.log').open('wb') as log:
        command(['make','-f','application.mk',f'-j{args.jobs}',f'APP_BIN_NAME={APP_NAME}',f'USER_SW_VER={version}',f'APP_VERSION={version}','TARGET_PLATFORM=bk7231n','OBK_VARIANT=0','CFG_USE_MQTT_TLS=0',f'ARM_GCC_TOOLCHAIN={gcc.parent}'],osdir,log)
        package(run,version,log,metadata,args.boot_trace,args.stock_layout)
    verify_stack(run,version,gcc)
    command([sys.executable,HERE/'verify_firmware.py','--run-dir',run])
    if not args.keep_build:shutil.rmtree(run/'build')
    print(f'Firmware and checksums: {run}/firmware')

if __name__=='__main__':main()
