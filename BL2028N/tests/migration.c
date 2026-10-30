/* Exercise the actual v1 loader and atomic v2 storage with disposable files. */
#include <assert.h>
#include "../src/ph_port.c"
void bl_mqtt_start(void){}
void bl_mqtt_diagnostics(ph_writer *w){(void)w;}
int main(int argc,char **argv){
 assert(argc==2);snprintf(store_dir,sizeof(store_dir),"%s",argv[1]);mkdir(store_dir,0700);init_lfs(1);
 legacy_record old;memset(&old,0,sizeof(old));old.magic=old.settings.magic=UINT32_C(0x314c4250);old.size=sizeof(old);
 old.settings.revision=19;old.settings.power_on=1;old.settings.indicator=1;old.settings.child_lock=1;old.settings.calibrated=1;
 old.settings.coefficients[0]=1.88;old.settings.coefficients[1]=2.25;old.settings.coefficients[2]=1.5;old.settings.energy_kwh=123.5;
 ph_auth_store_empty(&old.settings.auth);old.settings.auth.count=1;strcpy(old.settings.auth.pairs[0].peer,"001122334455");memset(old.settings.auth.pairs[0].key,9,16);old.settings.auth.pairs[0].scopes=7;
 old.crc=crc32(&old.settings,sizeof(old.settings));lfs_file_t f;assert(!lfs_file_open(&lfs,&f,".phicomm",LFS_O_WRONLY|LFS_O_CREAT));assert(lfs_file_write(&lfs,&f,&old,sizeof(old))==sizeof(old));assert(!lfs_file_close(&lfs,&f));
 ph_settings next;assert(load(&next));assert(next.magic==PH_STORE_MAGIC&&next.revision==19&&next.power_on&&next.child_lock&&next.calibrated&&next.indicator);
 assert(!memcmp(next.coefficients,old.settings.coefficients,sizeof(next.coefficients)));assert(next.energy_kwh==123.5);assert(!memcmp(&next.auth,&old.settings.auth,sizeof(next.auth)));assert(next.report_seconds==30&&!strcmp(next.name,"S30-V1B"));
 assert(save(NULL,&next));ph_settings reread;assert(load(&reread));assert(!memcmp(&next,&reread,sizeof(next)));
 PH_BL_Init(1);assert(bl_ready&&safe_boot&&!bl_device.relay&&bl_device.settings.power_on);
 assert(bl_device.settings.energy_kwh==123.5&&bl_device.settings.child_lock&&bl_device.settings.auth.count==1);
 assert(!lfs_file_open(&lfs,&f,".phicomm",LFS_O_WRONLY));old.crc^=1;assert(lfs_file_write(&lfs,&f,&old,sizeof(old))==sizeof(old));lfs_file_close(&lfs,&f);assert(!load(&reread));
 puts("PASS: v1 migration preserves credentials, energy, policy and calibration; v2 roundtrip; safe boot keeps relay off and saved policy; CRC corruption rejected");
}
