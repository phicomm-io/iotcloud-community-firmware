#include "signed_adapter.h"
#include "../../common/ota/mode.h"
#include "../../common/ota/signed_ota.h"
#include <string.h>
#define TC1_PRODUCT "PHICOMM_TC1_A1"
static signed_ota session;
static tc1_ota_metadata_t metadata;
void tc1_ota_metadata(tc1_ota_metadata_t *out);
static bool active, config_busy, unsigned_session, commit_uncertain;
static const char *ota_state="idle", *ota_error="";
static char transfer_error[96];
static mico_mutex_t lock;
static uint32_t expected, staged, wire_total, wire_done;
static unsigned ota_percent;
static uint16_t transport_crc;
static const uint8_t kernel_md5[16]={0xdf,0x26,0x41,0xdc,0xca,0x61,0xb3,0x0b,0xe1,0x54,0xb5,0x0b,0xcd,0xf2,0x50,0x03};
void tc1_ota_metadata(tc1_ota_metadata_t *out) {mico_rtos_lock_mutex(&lock);*out=metadata;mico_rtos_unlock_mutex(&lock);}
void tc1_ota_init(void) { mico_rtos_init_mutex(&lock); }
bool tc1_config_reserve(void) {
    mico_rtos_lock_mutex(&lock); bool ok=!active && !config_busy; if(ok)config_busy=true;
    mico_rtos_unlock_mutex(&lock);return ok;
}
void tc1_config_release(void) {mico_rtos_lock_mutex(&lock);config_busy=false;mico_rtos_unlock_mutex(&lock);}
bool tc1_ota_reserve(void) {
    mico_rtos_lock_mutex(&lock); bool ok=!active && !config_busy; if(ok) {active=true;ota_state="receiving";ota_error="";}
    mico_rtos_unlock_mutex(&lock); return ok;
}
void tc1_ota_snapshot(char *state,char *error,unsigned *progress) {
    mico_rtos_lock_mutex(&lock);snprintf(state,32,"%s",ota_state);
    snprintf(error,128,"%s",!strcmp(ota_state,"failed")?(*ota_error?ota_error:"OTA rejected"):"");
    *progress=ota_percent;
    mico_rtos_unlock_mutex(&lock);
}
void tc1_ota_abort(void) {
    mico_rtos_lock_mutex(&lock); if(!commit_uncertain && strcmp(ota_state,"ready"))active=false; if(strcmp(ota_state,"ready"))ota_state="failed"; mico_rtos_unlock_mutex(&lock);
}
void tc1_ota_failure(const char *reason) {
    mico_rtos_lock_mutex(&lock);
    if(!*ota_error){snprintf(transfer_error,sizeof(transfer_error),"%s",reason?reason:"Image validation failed");ota_error=transfer_error;}
    mico_rtos_unlock_mutex(&lock);
    tc1_ota_abort();
}
static uint16_t crc_step(uint16_t crc,const uint8_t *p,size_t n) {
    while(n--) { unsigned i; crc^=(uint16_t)*p++<<8;
        for(i=0;i<8;i++) crc=crc&0x8000 ? (crc<<1)^0x1021 : crc<<1;
    } return crc;
}
static bool erase(void *ctx) {
    (void)ctx; mico_logic_partition_t *part=MicoFlashGetInfo(MICO_PARTITION_OTA_TEMP);
    expected=unsigned_session?expected:session.image_length; staged=0; transport_crc=0;
    if(!part || expected<(unsigned_session?1u:0x64000u+144u) || expected>part->partition_length)return false;
#ifdef PHICOMM_DEVICE_OTA
    if(part->partition_length%4096)return false;
    for(uint32_t at=0;at<part->partition_length;at+=4096){
        if(ota_job_cancelled() || MicoFlashErase(MICO_PARTITION_OTA_TEMP,at,4096)!=kNoErr)return false;
        mico_thread_msleep(10);
    }
    return true;
#else
    return MicoFlashErase(MICO_PARTITION_OTA_TEMP,0,part->partition_length)==kNoErr;
#endif
}
static bool write_image(void *ctx,uint32_t offset,const uint8_t *data,size_t n) {
    (void)ctx; if(offset!=staged || n>expected-staged ||
        MicoFlashWrite(MICO_PARTITION_OTA_TEMP,&offset,(uint8_t *)data,n)!=kNoErr) return false;
    transport_crc=crc_step(transport_crc,data,n); staged+=n; return true;
}
static bool read_image(void *ctx,uint32_t offset,uint8_t *data,size_t n) {
    (void)ctx; return MicoFlashRead(MICO_PARTITION_OTA_TEMP,&offset,data,n)==kNoErr;
}
bool tc1_ota_begin(uint32_t total,bool rescue) {
    unsigned_session=rescue; expected=total;wire_total=total;wire_done=0;
    mico_rtos_lock_mutex(&lock);ota_percent=0;memset(&metadata,0,sizeof(metadata));mico_rtos_unlock_mutex(&lock);
    mico_logic_partition_t *part=MicoFlashGetInfo(MICO_PARTITION_OTA_TEMP);
    signed_ota_io io={erase,write_image,read_image,NULL};
    bool ok=part && (rescue?erase(NULL):signed_ota_begin(&session,total,part->partition_length,1,1,io));
    return ok;
}
bool tc1_ota_write(const uint8_t *data,size_t n) {
#ifdef PHICOMM_DEVICE_OTA
    if(ota_job_cancelled())return false;
#endif
    bool ok=unsigned_session?write_image(NULL,staged,data,n):signed_ota_write(&session,data,n);
    if(ok) {wire_done+=n;mico_rtos_lock_mutex(&lock);
        ota_percent=(unsigned)((uint64_t)wire_done*100/wire_total);
        if(!unsigned_session && session.authenticated) {
            const uint8_t *h=session.envelope;metadata.signature_verified=true;
            memcpy(metadata.version,h+64,32);
            metadata.key_id=(uint32_t)h[12]|(uint32_t)h[13]<<8|(uint32_t)h[14]<<16|(uint32_t)h[15]<<24;
            metadata.image_length=session.image_length;metadata.release_counter=0;
            for(unsigned i=0;i<8;i++)metadata.release_counter|=(uint64_t)h[24+i]<<(8*i);
        }
        mico_rtos_unlock_mutex(&lock);}
    return ok;
}
static uint32_t le32(const uint8_t *p)
{ return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t le16(const uint8_t *p) { return p[0] | ((uint16_t)p[1] << 8); }

static bool read_at(uint32_t offset, void *data, unsigned length)
{
    if (offset > expected || length > expected - offset) return false;
    return MicoFlashRead(MICO_PARTITION_OTA_TEMP, &offset, data, length) == kNoErr;
}

static bool md5_range(uint32_t start, uint32_t end, uint8_t digest[16])
{
    md5_context md5; InitMd5(&md5);
    uint8_t data[512];
    while (start < end) {
        unsigned n = end - start > sizeof(data) ? sizeof(data) : end - start;
        if (!read_at(start, data, n)) return false;
        Md5Update(&md5, data, n); start += n;
    }
    Md5Final(&md5, digest); return true;
}

/* MW300 exposes the running and passive slots through the MiCO API.
 * BOOTLOADER has MICO_FLASH_NONE on this platform; reading partition tables
 * through that partition always fails. Let the SDK own boot-table activation. */
static bool passive_slot_valid(void) {
    mico_logic_partition_t *current=MicoFlashGetInfo(MICO_PARTITION_APPLICATION);
    mico_logic_partition_t *target=MicoFlashGetInfo(MICO_PARTITION_OTA_TEMP);
    if(!current || !target)return false;
    uint32_t a=current->partition_start_addr,b=target->partition_start_addr;
    return current->partition_length==0xa9000 && target->partition_length==0xa9000 &&
        ((a==0x6000 && b==0xaf000)||(a==0xaf000 && b==0x6000));
}
bool tc1_ota_finish(void)
{
    const char *error = NULL;
    mico_rtos_lock_mutex(&lock);ota_state="validating";mico_rtos_unlock_mutex(&lock);
    /* Explicit raw compatibility mode follows the original SDK OTA flow.
     * Do not apply signed-image ABI, product, kernel or boot-table checks. */
    if (unsigned_session) {
        if (staged != expected) return false;
#ifdef PHICOMM_DEVICE_OTA
        if(!ota_job_commit())return false;
#endif
        if (mico_ota_switch_to_new_fw(expected, transport_crc) != kNoErr) {
            mico_rtos_lock_mutex(&lock);
            ota_state="failed"; commit_uncertain=true;
            mico_rtos_unlock_mutex(&lock);
            return false;
        }
        mico_rtos_lock_mutex(&lock); ota_state="ready"; mico_rtos_unlock_mutex(&lock);
        return true;
    }
    if (!unsigned_session && !signed_ota_finish(&session)) {
        return false;
    }
    mico_rtos_lock_mutex(&lock);metadata.image_verified=!unsigned_session;mico_rtos_unlock_mutex(&lock);
    uint8_t header[36], digest[16], tail[16], data[512];
    
    if (staged != expected) { error = "Truncated OTA body"; goto fail; }
    if (!read_at(0x64000u, header, sizeof(header))) { error = "OTA header read failed"; goto fail; }
    uint32_t payload = le32(header), pid = le32(header + 28);
    if (payload < 120 || payload > (276u*1024u) - 8 ||
        expected != 0x64000u + 8 + payload + 16 || le32(header + 8) != 0xc89346 ||
        le32(header + 16) != 1 || le16(header + 4) != le16(header + 6)) {
        error = "Invalid application header or ABI"; goto fail;
    }
    if (pid < 0x1f064000u + 8 || pid - 0x1f064000u > 8 + payload - sizeof(TC1_PRODUCT)) {
        error = "Product pointer outside application"; goto fail;
    }
    char product[sizeof(TC1_PRODUCT)];
    if(!read_at(0x64000u+pid-0x1f064000u,product,sizeof(product))) {error="Product read failed";goto fail;}
    if(memcmp(product,TC1_PRODUCT,sizeof(product)) && !unsigned_session) {error="Not a TC1 A1 image";goto fail;}
    uint16_t crc = 0;
    uint32_t position = 0x64000u + 8, end = expected - 16;
    while (position < end) {
        unsigned n = end - position > sizeof(data) ? sizeof(data) : end - position;
        if (!read_at(position, data, n)) { error = "OTA flash read failed"; goto fail; }
        crc = crc_step(crc, data, n); position += n;
    }
    if (crc != le16(header + 4)) { error = "Application CRC mismatch"; goto fail; }
    if (!md5_range(0, 0x64000u, digest) || memcmp(digest, kernel_md5, 16)) {
        error = "Incompatible MiCO kernel"; goto fail;
    }
    if (!md5_range(0, expected - 16, digest) || !read_at(expected - 16, tail, 16) || memcmp(digest, tail, 16)) {
        error = "OTA MD5 mismatch"; goto fail;
    }
    if(!passive_slot_valid()) {error="Invalid active/passive partitions";goto fail;}
#ifdef PHICOMM_DEVICE_OTA
    if(!ota_job_commit()){error="Cancelled";goto fail;}
#endif
    if(mico_ota_switch_to_new_fw(expected,transport_crc)!=kNoErr) {
        /* Commit may have partially persisted: inhibit further OTA until manual reboot. */
        mico_rtos_lock_mutex(&lock);ota_state="failed";commit_uncertain=true;mico_rtos_unlock_mutex(&lock);
        return false;
    }
    mico_rtos_lock_mutex(&lock);ota_state="ready";mico_rtos_unlock_mutex(&lock);
    /* Keep the reservation until reset, preventing another OTA from replacing the verified slot. */
    return true;
fail:
    mico_rtos_lock_mutex(&lock);ota_error=error;mico_rtos_unlock_mutex(&lock);return false;
}

