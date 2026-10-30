#!/usr/bin/env python3
"""Verify checksums, factory layout/CRC, OTA extraction and linked firmware."""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import zlib
from bk7231tools.crypto.code import BekenCodeCipher
from source_provenance import verify_sources

HERE=Path(__file__).resolve().parent
CACHE=Path.home()/'.cache/phicomm'

def crc16(data):
    crc=0xffff
    for byte in data:
        crc^=byte<<8
        for _ in range(8):crc=((crc<<1)^(0x8005 if crc&0x8000 else 0))&0xffff
    return crc

def fnv1a(data):
    value=2166136261
    for byte in data:value=((value^byte)*16777619)&0xffffffff
    return value

def c_string(field):
    # The native RBL packager writes C strings into fixed-width fields. Bytes
    # after the terminator are not part of the value and may contain 0xff.
    assert b'\0' in field, 'Unterminated RBL string field'
    return field.split(b'\0', 1)[0].decode('ascii')

def crc_blocks(encoded):
    assert len(encoded)%34==0
    decoded=[]
    for offset in range(0,len(encoded),34):
        block=encoded[offset:offset+34]
        assert crc16(block[:32])==int.from_bytes(block[32:],'big'),offset
        decoded.append(block[:32])
    return b''.join(decoded)

def partition(encoded,expected_name):
    # The packager adds CRC16 to payload blocks and the 96-byte RBL footer.
    # Unused flash between them stays 0xff, without block CRCs.
    footer=crc_blocks(encoded[-102:])
    fields=struct.unpack('<4sHHI16s24s24sIIIII',footer)
    magic,algorithm,_,_,name,_,_,_,fnv,raw_n,package_n,header_crc=fields
    assert magic==b'RBL\0' and algorithm==0 and name.rstrip(b'\0')==expected_name
    assert zlib.crc32(footer[:92])==header_crc
    assert package_n==(raw_n+31)//32*32
    end=package_n//32*34
    assert end<=len(encoded)-102 and all(byte==255 for byte in encoded[end:-102])
    payload=crc_blocks(encoded[:end])
    assert fnv1a(payload[:raw_n])==fnv
    return payload[:raw_n]

def main():
    verify_sources()
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--run-dir',type=Path,required=True);args=p.parse_args();run=args.run_dir.resolve();folder=run/'firmware'
    manifest=json.loads((run/'build-manifest.json').read_text());v=manifest['version']
    for name,digest in manifest['source_sha256'].items():assert hashlib.sha256((HERE/name).read_bytes()).hexdigest()==digest,name
    sizes={}
    for line in (folder/'SHA256SUMS').read_text().splitlines():
        digest,name=line.split('  ',1);data=(folder/name).read_bytes();assert hashlib.sha256(data).hexdigest()==digest,name;sizes[name]=len(data)
    raw=(folder/f'BL2028N_app_{v}.bin').read_bytes()
    assert (b'[S30-BOOT] ' in raw)==bool(manifest.get('boot_trace',False)), 'Boot trace does not match build manifest'
    packages={}
    stock_layout=manifest.get('flash_layout')
    flavors=[] if stock_layout else [('BK7231N','bk7231n_bootloader_enc.bin'),('BK7231M','bk7231n_bootloader.bin')]
    if stock_layout:
        from stock_layout import LAYOUT
        assert stock_layout==LAYOUT
        assert not any('_QIO_' in name or ('_UA_' in name and '_STOCK_UA_' not in name) for name in sizes)
        assert (len(raw)+31)//32*34<=LAYOUT['app_end']-LAYOUT['app_start']-102
    for flavor,boot in flavors:
        qio=(folder/f'BL2028N_{flavor}_QIO_{v}.bin').read_bytes();ua=(folder/f'BL2028N_{flavor}_UA_{v}.bin').read_bytes()
        loader=(CACHE/'openbeken/bk7231n-upstream/platforms/bk7231n/bk7231n_os/tools/generate'/boot).read_bytes()
        assert len(qio)==1220464 and len(ua)==1150832
        assert qio[0x11000:]==ua
        assert partition(qio[:0x11000],b'bootloader')==loader
        payload=partition(ua,b'app')
        code_length=(len(raw)+15)//32*32+16
        assert len(payload)==code_length
        assert c_string(crc_blocks(ua[-102:])[28:52])==v
        coefficients=(0x510fb093,0xa3cbeadc,0x5993a17e,0xc7adeb03) if flavor=='BK7231N' else (0,0,0,0)
        cipher=BekenCodeCipher(coefficients)
        assert bytes(cipher.decrypt(cipher.pad(payload),stream_start_offset=0x10000)[:code_length])==raw.ljust(code_length,b'\xff')
        packages[flavor]=ua
    if not stock_layout:assert packages['BK7231N']!=packages['BK7231M']
    rbl=(folder/f'BL2028N_OTA_{v}.rbl').read_bytes();ug=(folder/f'BL2028N_UG_{v}.bin').read_bytes()
    magic,algorithm,_,stamp,name,version,sn,payload_crc,fnv,raw_n,package_n,header_crc=struct.unpack('<4sHHI16s24s24sIIIII',rbl[:96])
    assert magic==b'RBL\0' and algorithm==0x102 and name.startswith(b'app\0')
    assert c_string(version)==v
    assert zlib.crc32(rbl[:92])==header_crc and zlib.crc32(rbl[96:])==payload_crc
    assert len(rbl)==96+package_n and raw_n==len(raw)
    assert fnv1a(raw)==fnv
    if stock_layout:
        ua=(folder/f'BL2028N_STOCK_UA_{v}.bin').read_bytes()
        assert len(ua)==LAYOUT['app_end']-LAYOUT['app_start']
        end=(len(raw)+31)//32*34
        assert crc_blocks(ua[:end])==raw.ljust((len(raw)+31)//32*32,b'\xff')
        assert all(b==255 for b in ua[end:-102])
        assert crc_blocks(ua[-102:])==rbl[:96]
    decrypted=subprocess.run(['openssl','enc','-d','-aes-256-cbc','-K',b'0123456789ABCDEF0123456789ABCDEF'.hex(),'-iv',b'0123456789ABCDEF'.hex()],input=rbl[96:],capture_output=True,check=True).stdout
    assert gzip.decompress(decrypted)==raw
    assert ug[:4]==bytes.fromhex('55aa55aa') and ug[28:32]==bytes.fromhex('aa55aa55') and ug[32:]==rbl
    assert struct.unpack('>III',ug[16:28])==(len(rbl),sum(rbl)&0xffffffff,sum(ug[:24])&0xffffffff)
    # Default 32 KiB LFS starts at 0x1c8000; preserve it with this OTA image.
    ota_start=0x132000 if stock_layout else 0x12a000
    ota_end=0x1e0000 if stock_layout else 0x1c8000
    rounded=((len(rbl)+4095)//4096)*4096
    if stock_layout:assert ota_start+rounded<=ota_end
    else:assert ota_start+rounded<ota_end
    elf=run/f'OpenBK7231N_{v}.axf'
    symbols=subprocess.check_output([CACHE/'gcc5/bin/arm-none-eabi-nm',elf],text=True)
    for symbol in ('PH_BL_Init','PH_BL_Tick','PH_BL_StartHTTP','PH_BL_FactoryMac','ph_command','ph_meter_byte','ph_udp_handle','ph_http_server_step','ph_mqtt_session_run','ph_mqtt_core_init','MQTTConnect'):
        assert any(line.endswith(' T '+symbol) or line.endswith(' t '+symbol) for line in symbols.splitlines()),symbol
    for forbidden in ('cJSON_','json_object','cmnd_JsonCommand','mico_rtos','esphome','Arduino'):
        assert forbidden not in symbols,forbidden
    size=subprocess.check_output([CACHE/'gcc5/bin/arm-none-eabi-size',elf],text=True)
    (run/'size.log').write_text(size)
    result={'status':'PASS','source_hashes':'PASS','firmware_sha256':'PASS','qio_ua_layout_and_crc':'PASS','n_and_zero_key_cipher_roundtrip':'PASS','deterministic_application_padding':'PASS','rbl_crc_fnv_aes_gzip_roundtrip':'PASS','ug_header':'PASS','ota_preserves_default_littlefs':'PASS','required_symbols':'PASS','no_legacy_json_or_other_framework_symbols':'PASS','sizes':sizes,'hardware_verified':False}
    if stock_layout:
        for key in ['qio_ua_layout_and_crc','n_and_zero_key_cipher_roundtrip','deterministic_application_padding']:
            result[key]='NOT_EMITTED: preserves the device stock bootloader'
        result['stock_layout']=stock_layout
        result['stock_uart_application_crc_and_footer']='PASS'
        result['stock_uart_write_start']='0x11000'
    (run/'firmware-validation.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2));print(size)

if __name__=='__main__':main()
