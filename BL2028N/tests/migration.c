/* Actual v1/v2 importer, fixed old byte fixtures and atomic v3 failure paths. */
#include <assert.h>
#include <stdlib.h>
#include "platform_host.h"
static int fail_stage;
static size_t record_allocations;
static void *migration_malloc(size_t);
static void migration_free(void *);
static lfs_file_t *migration_output;
static int migration_open(lfs_t *,lfs_file_t *,const char *,int);
static lfs_ssize_t migration_write(lfs_t *,lfs_file_t *,const void *,size_t);
static int migration_sync(lfs_t *,lfs_file_t *);
static int migration_close(lfs_t *,lfs_file_t *);
static int migration_rename(lfs_t *,const char *,const char *);
#define lfs_file_open migration_open
#define lfs_file_write migration_write
#define lfs_file_sync migration_sync
#define lfs_file_close migration_close
#define lfs_rename migration_rename
#define malloc migration_malloc
#define free migration_free
#include "../src/ph_port.c"
#undef lfs_file_open
#undef lfs_file_write
#undef lfs_file_sync
#undef lfs_file_close
#undef lfs_rename
#undef malloc
#undef free
#include "fixtures/s30-legacy-records.h"
static void *migration_malloc(size_t n) {
 assert(n==sizeof(disk_record));if(fail_stage==6)return NULL;
 void *p=malloc(n);assert(p);++record_allocations;assert(record_allocations==1);return p;
}
static void migration_free(void *p) {
 assert(p&&record_allocations==1);--record_allocations;free(p);
}
void bl_mqtt_start(void){}
void bl_mqtt_diagnostics(ph_writer *w){(void)w;}
static int migration_open(lfs_t *fs,lfs_file_t *f,const char *name,int flags) {
 bool output=!strcmp(name,".phicomm.tmp");
 if(output&&fail_stage==1)return -1;
 int rc=lfs_file_open(fs,f,name,flags);if(!rc&&output)migration_output=f;return rc;
}
static lfs_ssize_t migration_write(lfs_t *fs,lfs_file_t *f,const void *data,size_t n) {
 return lfs_file_write(fs,f,data,n-(f==migration_output&&fail_stage==2));
}
static int migration_sync(lfs_t *fs,lfs_file_t *f) {
 return f==migration_output&&fail_stage==3?-1:lfs_file_sync(fs,f);
}
static int migration_close(lfs_t *fs,lfs_file_t *f) {
 bool output=f==migration_output;int rc=lfs_file_close(fs,f);if(output)migration_output=NULL;
 return output&&fail_stage==4?-1:rc;
}
static int migration_rename(lfs_t *fs,const char *a,const char *b) {
 return fail_stage==5?-1:lfs_rename(fs,a,b);
}
static void write_bytes(const void *bytes,size_t n) {
 lfs_file_t f;assert(!lfs_file_open(&lfs,&f,".phicomm",LFS_O_WRONLY|LFS_O_CREAT|LFS_O_TRUNC));
 assert(lfs_file_write(&lfs,&f,bytes,n)==(lfs_ssize_t)n);assert(!lfs_file_close(&lfs,&f));
}
static size_t read_bytes(unsigned char *out,size_t cap) {
 lfs_file_t f;assert(!lfs_file_open(&lfs,&f,".phicomm",LFS_O_RDONLY));
 lfs_soff_t n=lfs_file_size(&lfs,&f);assert(n>=0&&(size_t)n<=cap);
 assert(lfs_file_read(&lfs,&f,out,(size_t)n)==n);assert(!lfs_file_close(&lfs,&f));return (size_t)n;
}
static void assert_old_file(const unsigned char *fixture,size_t n) {
 unsigned char actual[sizeof(disk_record)+sizeof(ph_store_v2_record)];assert(read_bytes(actual,sizeof(actual))==n);
 assert(!memcmp(actual,fixture,n));
}
static void assert_imported(const ph_settings *s,unsigned version) {
 assert(ph_settings_valid(s)&&s->magic==PH_STORE_MAGIC&&s->revision==2718);
 assert(s->child_lock==1&&s->indicator==0&&s->power_on==1&&s->calibrated==1);
 assert(s->coefficients[0]==1.23456789&&s->coefficients[1]==2.34567891&&s->coefficients[2]==3.45678912&&s->energy_kwh==876.125);
 assert(s->auth.count==12);
 for(unsigned i=0;i<12;++i) {
  char peer[13];snprintf(peer,sizeof(peer),"00112233%04x",i);
  assert(!strcmp(s->auth.pairs[i].peer,peer)&&s->auth.pairs[i].scopes==i%7+1);
  for(unsigned k=0;k<16;++k)assert(s->auth.pairs[i].key[k]==(uint8_t)(i*17+k+1));
 }
#ifdef PH_AUTH_HTTP_STATELESS
 assert(!s->auth.remote_count&&!s->auth.reserved);
 for(unsigned i=0;i<sizeof(s->auth.remote);++i)assert(!((const unsigned char *)s->auth.remote)[i]);
#endif
 if(version==2) {
  assert(s->report_seconds==37&&strlen(s->name)==63&&strlen(s->socket_name)==63);
  for(unsigned i=0;i<63;++i)assert(s->name[i]=='N'&&s->socket_name[i]=='S');
 } else assert(s->report_seconds==30&&!strcmp(s->name,"S30-V1B")&&!strcmp(s->socket_name,"S30-V1B"));
}
static void fixture_migration_tests(void) {
 CFG_SetWiFiSSID("kept-wifi");CFG_SetWiFiPass("kept-wifi-password");CFG_SetWiFiSSID2("kept-backup");
 CFG_SetMQTTUserName("kept-mqtt");CFG_SetMQTTPass("kept-mqtt-password");CFG_SetWebPassword("kept-admin-password");
 for(unsigned version=1;version<=2;++version) {
  const unsigned char *fixture=version==1?s30_legacy_v1:s30_legacy_v2;
  size_t n=version==1?sizeof(s30_legacy_v1):sizeof(s30_legacy_v2);
  for(unsigned stage=1;stage<=6;++stage) {
   write_bytes(fixture,n);fail_stage=stage;ph_settings imported;assert(load(&imported));assert_imported(&imported,version);
   assert_old_file(fixture,n);assert(!record_allocations); /* Every failed save stage leaves old bytes. */
   fail_stage=0;assert(load(&imported));assert_imported(&imported,version);
   unsigned char actual[sizeof(disk_record)];assert(read_bytes(actual,sizeof(actual))==sizeof(disk_record));
   disk_record r;memcpy(&r,actual,sizeof(r));assert(r.magic==PH_STORE_MAGIC&&r.size==sizeof(r)&&r.credential_magic==0);
   ph_settings reread;assert(load(&reread));assert_imported(&reread,version);assert(!memcmp(&reread,&imported,sizeof(reread)));
  }
 }
 assert(!strcmp(CFG_GetWebPassword(),"kept-admin-password")&&!strcmp(CFG_GetWiFiSSID(),"kept-wifi")&&!strcmp(CFG_GetWiFiPass(),"kept-wifi-password")&&!strcmp(CFG_GetWiFiSSID2(),"kept-backup")&&!strcmp(CFG_GetMQTTUserName(),"kept-mqtt")&&!strcmp(CFG_GetMQTTPass(),"kept-mqtt-password"));
 puts("PASS: fixed v1/v2 bytes preserve pairs, calibration, energy, flags and v2 names/report; allocation/open/write/sync/close/rename failures retain old bytes and retry into v4; framework credentials unchanged");
}
static void rejection_tests(void) {
 unsigned char corrupt[sizeof(s30_legacy_v2)];ph_settings s;
 for(unsigned variant=0;variant<9;++variant) {
  memcpy(corrupt,s30_legacy_v2,sizeof(corrupt));
  ph_store_v2_record r;memcpy(&r,corrupt,sizeof(r));
  switch(variant) {
   case 0:r.magic^=1;break;
   case 1:r.settings.magic^=1;break;
   case 2:r.size--;break;
   case 3:r.crc^=1;break;
   case 4:r.settings.child_lock=2;break;
   case 5:r.settings.auth.count=13;break;
   case 6:r.settings.coefficients[0]=NAN;break;
   case 7:memset(r.settings.name,'X',sizeof(r.settings.name));break;
   case 8:r.settings.auth.pairs[0].scopes=8;break;
  }
  if(variant>3)r.crc=crc32(&r.settings,sizeof(r.settings));
  memcpy(corrupt,&r,sizeof(r));write_bytes(corrupt,sizeof(corrupt));assert(!load(&s));assert_old_file(corrupt,sizeof(corrupt));
 }
 write_bytes(s30_legacy_v2,sizeof(s30_legacy_v2)-1);assert(!load(&s));assert_old_file(s30_legacy_v2,sizeof(s30_legacy_v2)-1);
 puts("PASS: exact magic/size/CRC and semantic validation reject malformed legacy files without rewriting");
}
static void meter_default_migration_tests(void) {
 for(unsigned version=1;version<=3;++version)for(unsigned variant=0;variant<4;++variant) {
  ph_settings input,expected,actual;ph_settings_default(&input);input.revision=19;input.energy_kwh=123.5;input.power_on=1;input.child_lock=1;
  if(variant!=3){input.coefficients[0]=1.88;input.coefficients[1]=1;input.coefficients[2]=1.88;}
  if(variant==1)input.calibrated=1;
  if(variant==2)input.coefficients[1]=2.25;
  expected=input;if(variant==0){expected.coefficients[0]=1.894288;expected.coefficients[1]=1.0102;expected.coefficients[2]=1.88835008906368;}
  if(version==3)assert(save(NULL,&input));
  else {
   ph_store_v2_record old;memset(&old,0,sizeof(old));old.magic=version==1?PH_STORE_V1_MAGIC:PH_STORE_V2_MAGIC;
   old.size=version==1?sizeof(ph_store_v1_record):sizeof(old);old.settings.magic=old.magic;
   old.settings.revision=input.revision;old.settings.child_lock=input.child_lock;old.settings.indicator=input.indicator;old.settings.power_on=input.power_on;old.settings.calibrated=input.calibrated;
   memcpy(old.settings.coefficients,input.coefficients,sizeof(input.coefficients));old.settings.energy_kwh=input.energy_kwh;
   old.settings.auth.magic=PH_AUTH_STORE_MAGIC;old.settings.auth.version=1;
   old.settings.report_seconds=input.report_seconds;memcpy(old.settings.name,input.name,sizeof(input.name));memcpy(old.settings.socket_name,input.socket_name,sizeof(input.socket_name));
   old.crc=crc32(&old.settings,version==1?sizeof(ph_store_v1_settings):sizeof(old.settings));write_bytes(&old,old.size);
  }
  assert(load(&actual));assert(!memcmp(&actual,&expected,sizeof(actual)));assert(load(&actual));assert(!memcmp(&actual,&expected,sizeof(actual)));
 }
 puts("PASS: v1/v2/v3 default meter upgrade, custom/explicit coefficients and nonzero energy preserved");
}
static void current_record_tests(void) {
 ph_settings input,actual;ph_settings_default(&input);input.revision=9001;
 input.auth.count=12;
 for(unsigned i=0;i<12;++i) {
  ph_auth_pair_record *p=&input.auth.pairs[i];snprintf(p->peer,sizeof(p->peer),"22334455%04x",i);
  p->scopes=1+i%7;for(unsigned k=0;k<16;++k)p->key[k]=(uint8_t)(i*13+k+1);
 }
#ifdef PH_AUTH_HTTP_STATELESS
 input.auth.remote_count=12;
 for(unsigned i=0;i<12;++i) {
  ph_auth_pair_record *p=&input.auth.remote[i];snprintf(p->peer,sizeof(p->peer),"66778899%04x",i);
  p->scopes=1+(i+3)%7;for(unsigned k=0;k<16;++k)p->key[k]=(uint8_t)(i*19+k+1);
 }
#endif
 assert(save(NULL,&input)&&load(&actual)&&!memcmp(&actual,&input,sizeof(input)));
 disk_record record;assert(read_bytes((unsigned char *)&record,sizeof(record))==sizeof(record));
 /* A valid CRC cannot turn an unknown credential format into a usable file. */
 record.credential_magic=PH_CREDENTIAL_MAGIC^1;
 record.crc=crc32(&record.settings,sizeof(record)-offsetof(disk_record,settings));
 write_bytes(&record,sizeof(record));assert(!load(&actual));
 record.credential_magic=PH_CREDENTIAL_MAGIC;memset(record.password,'X',sizeof(record.password));
 record.crc=crc32(&record.settings,sizeof(record)-offsetof(disk_record,settings));
 write_bytes(&record,sizeof(record));assert(!load(&actual));
 assert(save(NULL,&input));
 puts("PASS: current record roundtrips every inbound/outbound slot and rejects invalid credential metadata even with a matching CRC");
}
static void ota_checkpoint_tests(void) {
 ph_settings input,actual;ph_settings_default(&input);input.energy_kwh=123.5;bl_device.settings=input;ph_meter_init(&bl_device.meter,12.5);energy_saved_at=42;
 unsigned char packed[128];ph_writer writer;struct printbuf buffer;assert(phw_init_fixed(&writer,PH_WIRE_MSGPACK,&buffer,packed,sizeof(packed)));
 phw_begin(&writer,NULL,false);phw_text(&writer,"url","http://127.0.0.1/test.rbl");phw_end(&writer);
 ph_value params;assert(phv_parse(PH_WIRE_MSGPACK,packed,phw_finish_fixed(&writer),&params));
 fail_stage=2;assert(!strcmp(platform_action(NULL,"ota.start",&params,&writer,true),"storage_error"));
 assert(!bl_ota_pending&&!bl_device.maintenance&&!*ota_url&&energy_saved_at==42&&bl_device.settings.energy_kwh==123.5);
 fail_stage=0;assert(!strcmp(platform_action(NULL,"ota.start",&params,&writer,true),"accepted"));
 assert(bl_ota_pending&&bl_device.maintenance&&load(&actual)&&actual.energy_kwh==12.5);
 bl_ota_pending=false;ota_url[0]=0;ph_device_set_maintenance(&bl_device,false);
 puts("PASS: OTA commits current energy before acceptance and leaves maintenance off on persistence failure");
}
static void v3_migration_tests(void) {
 ph_store_v3_record old;memset(&old,0,sizeof(old));
 old.magic=old.settings.magic=PH_STORE_V3_MAGIC;old.size=sizeof(old);
 old.settings.revision=33;old.settings.indicator=1;old.settings.power_on=1;old.settings.calibrated=1;
 old.settings.coefficients[0]=1.25;old.settings.coefficients[1]=2.5;old.settings.coefficients[2]=3.75;
 old.settings.energy_kwh=123.5;old.settings.report_seconds=7;strcpy(old.settings.name,"Desk");strcpy(old.settings.socket_name,"Printer");
 old.settings.auth.magic=PH_AUTH_STORE_MAGIC;old.settings.auth.version=PH_AUTH_STORE_VERSION;
 old.settings.auth.count=old.settings.auth.remote_count=12;
 for(unsigned i=0;i<12;++i) {
  ph_store_old_pair *a=&old.settings.auth.pairs[i],*b=&old.settings.auth.remote[i];
  snprintf(a->peer,sizeof(a->peer),"22334455%04x",i);snprintf(b->peer,sizeof(b->peer),"66778899%04x",i);
  memset(a->key,i+1,16);memset(b->key,i+33,16);a->scopes=b->scopes=1+i%7;
 }
 old.credential_magic=PH_CREDENTIAL_MAGIC;strcpy(old.password,"test-admin-password");
 old.crc=crc32(&old.settings,sizeof(old)-offsetof(ph_store_v3_record,settings));
 for(unsigned stage=1;stage<=6;++stage) {
  write_bytes(&old,sizeof(old));fail_stage=stage;ph_settings actual;assert(load(&actual));
  assert(actual.magic==PH_STORE_MAGIC&&actual.power_next_id==1&&!actual.last_relay);
  assert(actual.auth.count==12&&actual.auth.remote_count==12&&!memcmp(&actual.auth,&old.settings.auth,sizeof(actual.auth)));
  assert(actual.revision==33&&actual.report_seconds==7&&actual.energy_kwh==123.5&&!strcmp(actual.name,"Desk")&&!strcmp(actual.socket_name,"Printer"));
  assert(credential_owned&&!strcmp(committed_password,old.password)&&!strcmp(CFG_GetWebPassword(),old.password));
  assert_old_file((const unsigned char *)&old,sizeof(old));assert(!record_allocations);fail_stage=0;
  assert(load(&actual));ph_settings reread;assert(load(&reread)&&!memcmp(&reread,&actual,sizeof(actual)));
  disk_record current;assert(read_bytes((unsigned char *)&current,sizeof(current))==sizeof(current));
  assert(current.magic==PH_STORE_MAGIC&&current.credential_magic==PH_CREDENTIAL_MAGIC&&!strcmp(current.password,old.password));
 }
 old.password[0]=1;old.crc=crc32(&old.settings,sizeof(old)-offsetof(ph_store_v3_record,settings));
 write_bytes(&old,sizeof(old));ph_settings actual;assert(!load(&actual));assert_old_file((const unsigned char *)&old,sizeof(old));
 puts("PASS: frozen rc.5 v3 imports pairs, password, names, calibration and energy; six failed-save stages preserve old bytes and retry to v4 without leaking records");
}
static void save_failure_tests(void) {
 ph_settings original,next;ph_settings_default(&original);original.energy_kwh=123.5;
 next=original;next.revision++;strcpy(next.name,"Changed");
 for(unsigned stage=1;stage<=6;++stage) {
  fail_stage=0;assert(save(NULL,&original));
  unsigned char before[sizeof(disk_record)];assert(read_bytes(before,sizeof(before))==sizeof(before));
  fail_stage=stage;assert(!save(NULL,&next));assert(!record_allocations&&!migration_output);
  assert_old_file(before,sizeof(before));
  fail_stage=0;assert(save(NULL,&next));ph_settings actual;assert(load(&actual));
  assert(!memcmp(&actual,&next,sizeof(actual))&&!record_allocations);
 }
 /* Invalid overrides must free their temporary record before returning. */
 assert(!save_record(&original,NULL,-1,NULL,false)&&!record_allocations);
 puts("PASS: current-record allocation and every I/O failure preserve committed bytes, release memory and recover on retry");
}
static void restore_policy_tests(void) {
 ph_settings input,actual;ph_settings_default(&input);input.power_on=PH_BOOT_RESTORE;input.last_relay=0;
 input.power_rules[0]=(ph_power_rule){.id=1,.enabled=1,.comparison=PH_POWER_BELOW,.power_milliwatts=5000,.seconds=600,.target=255,.on=0};input.power_next_id=2;
 assert(save(NULL,&input)&&load(&actual)&&!memcmp(&input,&actual,sizeof(input)));
 ph_device_ops ops={NULL,now,save,set_relay,reboot,network_info,platform_action,diagnostics,network_stamp};
 assert(ph_device_init(&bl_device,&ops,&actual,"aabbccddeeff","1.0.0-rc.5-test",99)&&!bl_device.relay);
 assert(ph_local_toggle(&bl_device)&&bl_device.relay);assert(checkpoint(1000,true));
 assert(load(&actual)&&actual.last_relay&&actual.power_on==PH_BOOT_RESTORE);
 assert(ph_device_init(&bl_device,&ops,&actual,"aabbccddeeff","1.0.0-rc.5-test",100)&&bl_device.relay);
 assert(ph_local_toggle(&bl_device)&&!bl_device.relay);fail_stage=2;
 assert(!checkpoint(2000,true));assert(load(&actual)&&actual.last_relay);
 fail_stage=0;assert(checkpoint(3000,true));assert(load(&actual)&&!actual.last_relay);
 puts("PASS: rules and restore policy roundtrip; local relay checkpoint survives reboot and failed writes retry");
}
int main(int argc,char **argv) {
 assert(argc==2);snprintf(store_dir,sizeof(store_dir),"%s",argv[1]);mkdir(store_dir,0700);init_lfs(1);
 fixture_migration_tests();rejection_tests();meter_default_migration_tests();current_record_tests();ota_checkpoint_tests();v3_migration_tests();save_failure_tests();restore_policy_tests();
 write_bytes(s30_legacy_v2,sizeof(s30_legacy_v2));PH_BL_Init(1);
 assert(bl_ready&&safe_boot&&!bl_device.relay&&bl_device.settings.power_on);assert_imported(&bl_device.settings,2);
 assert(!record_allocations);puts("PASS: safe boot retains saved policy and all migrated settings with relay off");
}
