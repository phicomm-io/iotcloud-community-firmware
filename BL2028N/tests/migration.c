/* Exercise the actual v1 loader and atomic v2 storage with disposable files. */
#include <assert.h>
#include "../src/ph_port.c"
void bl_mqtt_start(void){}
void bl_mqtt_diagnostics(ph_writer *w){(void)w;}
static void write_settings(const ph_settings *s,bool legacy){
 disk_record current={0};legacy_record old={0};
 current.magic=PH_STORE_MAGIC;current.size=sizeof(current);current.settings=*s;
 current.crc=crc32(&current.settings,sizeof(current.settings));
 old.magic=old.settings.magic=UINT32_C(0x314c4250);old.size=sizeof(old);
 old.settings.revision=s->revision;old.settings.power_on=s->power_on;
 old.settings.child_lock=s->child_lock;old.settings.indicator=s->indicator;old.settings.calibrated=s->calibrated;
 memcpy(old.settings.coefficients,s->coefficients,sizeof(s->coefficients));
 old.settings.energy_kwh=s->energy_kwh;old.settings.auth=s->auth;
 old.crc=crc32(&old.settings,sizeof(old.settings));
 const void *record=legacy?(const void*)&old:(const void*)&current;
 size_t size=legacy?sizeof(old):sizeof(current);lfs_file_t f;
 assert(!lfs_file_open(&lfs,&f,".phicomm",LFS_O_WRONLY|LFS_O_CREAT|LFS_O_TRUNC));
 assert(lfs_file_write(&lfs,&f,record,size)==(lfs_ssize_t)size);assert(!lfs_file_close(&lfs,&f));
}
static void meter_default_migration_tests(void){
 for(unsigned legacy=0;legacy<2;++legacy)for(unsigned variant=0;variant<4;++variant){
  ph_settings input,expected,actual;ph_settings_default(&input);
  assert(input.coefficients[0]==1.894288&&input.coefficients[1]==1.0102&&input.coefficients[2]==1.88835008906368);
  input.revision=19;input.energy_kwh=123.5;input.power_on=1;input.child_lock=1;
  input.auth.count=1;strcpy(input.auth.pairs[0].peer,"001122334455");memset(input.auth.pairs[0].key,9,16);input.auth.pairs[0].scopes=7;
  if(variant!=3){input.coefficients[0]=1.88;input.coefficients[1]=1;input.coefficients[2]=1.88;}
  if(variant==1)input.calibrated=1; /* Explicitly saved old values must win. */
  if(variant==2)input.coefficients[1]=2.25; /* Preserve an unflagged custom value. */
  expected=input;
  if(variant==0){expected.coefficients[0]=1.894288;expected.coefficients[1]=1.0102;expected.coefficients[2]=1.88835008906368;}
  write_settings(&input,legacy);assert(load(&actual));assert(!memcmp(&expected,&actual,sizeof(expected)));
  assert(load(&actual));assert(!memcmp(&expected,&actual,sizeof(expected))); /* Idempotent readback. */
 }
 ph_settings input,actual;ph_settings_default(&input);
 input.coefficients[0]=input.coefficients[2]=1.88;input.coefficients[1]=1;
 write_settings(&input,false);
 char blocked[1200];snprintf(blocked,sizeof(blocked),"%s/.phicomm.tmp",store_dir);assert(!mkdir(blocked,0700));
 assert(load(&actual));assert(actual.coefficients[0]==1.894288); /* Still boots with corrected values. */
 disk_record unchanged;lfs_file_t f;assert(!lfs_file_open(&lfs,&f,".phicomm",LFS_O_RDONLY));
 assert(lfs_file_read(&lfs,&f,&unchanged,sizeof(unchanged))==sizeof(unchanged));assert(!lfs_file_close(&lfs,&f));
 assert(!memcmp(&unchanged.settings,&input,sizeof(input))); /* Failed migration never replaces the old record. */
 assert(!rmdir(blocked));assert(load(&actual));assert(actual.coefficients[0]==1.894288);
 assert(load(&input));assert(!memcmp(&input,&actual,sizeof(input))); /* Retry persisted successfully. */
 puts("PASS: stock model defaults; v1/v2 uncalibrated migration; custom/explicit values preserved; failed save is retryable");

 bl_device.settings=input;ph_meter_init(&bl_device.meter,12.5);energy_saved_at=42;
 unsigned char packed[128];ph_writer writer;struct printbuf buffer;
 assert(phw_init_fixed(&writer,PH_WIRE_MSGPACK,&buffer,packed,sizeof(packed)));
 phw_begin(&writer,NULL,false);phw_text(&writer,"url","http://127.0.0.1/test.rbl");phw_end(&writer);
 ph_value params;assert(phv_parse(PH_WIRE_MSGPACK,packed,phw_finish_fixed(&writer),&params));
 assert(!mkdir(blocked,0700));
 assert(!strcmp(platform_action(NULL,"ota.start",&params,&writer,true),"storage_error"));
 assert(!bl_ota_pending&&!bl_device.maintenance&&!*ota_url&&energy_saved_at==42);
 assert(bl_device.settings.energy_kwh==input.energy_kwh);
 assert(!rmdir(blocked));assert(!strcmp(platform_action(NULL,"ota.start",&params,&writer,true),"accepted"));
 assert(bl_ota_pending&&bl_device.maintenance);assert(load(&actual)&&actual.energy_kwh==12.5);
 bl_ota_pending=false;ota_url[0]=0;ph_device_set_maintenance(&bl_device,false);
 puts("PASS: OTA saves current energy before acceptance; storage failure does not enter maintenance or start a download");
}
int main(int argc,char **argv){
 assert(argc==2);snprintf(store_dir,sizeof(store_dir),"%s",argv[1]);mkdir(store_dir,0700);init_lfs(1);
 meter_default_migration_tests();
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
