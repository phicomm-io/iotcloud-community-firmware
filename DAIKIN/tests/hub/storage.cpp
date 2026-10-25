/* SPDX-License-Identifier: GPL-3.0-or-later */
// Compile the exact production layout and NVS guards against a bounded Flash
// and power-cycleable NVS model, without requiring the ESP32 SDK or hardware.
#define IOTCLOUD_HOME_STORAGE_HOST_TEST
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>
using esp_err_t=int;
using nvs_handle=unsigned;
constexpr int ESP_OK=0, ESP_FAIL=-1, ESP_ERR_NVS_NOT_FOUND=0x1102;
constexpr int ESP_PARTITION_TYPE_APP=0,ESP_PARTITION_TYPE_DATA=1,ESP_PARTITION_TYPE_ANY=0xff;
constexpr int ESP_PARTITION_SUBTYPE_APP_OTA_0=0x10,ESP_PARTITION_SUBTYPE_APP_OTA_1=0x11;
constexpr int ESP_PARTITION_SUBTYPE_DATA_OTA=0,ESP_PARTITION_SUBTYPE_DATA_PHY=1;
constexpr int ESP_PARTITION_SUBTYPE_DATA_NVS=2,ESP_PARTITION_SUBTYPE_ANY=0xff;
struct esp_flash_t {};
struct esp_partition_t {
  esp_flash_t *flash_chip=nullptr;
  int type=0,subtype=0;
  uint32_t address=0,size=0;
  char label[17]={};
  bool encrypted=false;
};
struct Iterator {size_t index;};
using esp_partition_iterator_t=Iterator *;
static esp_flash_t chip,otherChip;
static esp_flash_t *esp_flash_default_chip=&chip;
static std::array<uint8_t,0x400000> flash;
static std::vector<esp_partition_t> partitions;
static uint32_t flashSize;
static unsigned reads,writes,erases,inits,iterators,finds,readFailAt,findFailAt;
static bool readFail,writeFail,verifyFail,eraseFail,initFail,sizeFail;
using Blob=std::vector<uint8_t>;
static std::map<std::string,Blob> durable,staged;
static esp_err_t esp_flash_get_size(esp_flash_t *,uint32_t *size) {
  *size=flashSize;return sizeFail?ESP_FAIL:ESP_OK;
}
static const esp_partition_t *esp_partition_find_first(int type,int subtype,const char *) {
  for(const auto &p:partitions)if(p.type==type&&p.subtype==subtype)return &p;
  return nullptr;
}
static esp_partition_iterator_t esp_partition_find(int,int,const char *) {
  ++finds;
  if(partitions.empty() || finds==findFailAt)return nullptr;
  ++iterators;return new Iterator{0};
}
static const esp_partition_t *esp_partition_get(esp_partition_iterator_t it) {return &partitions.at(it->index);}
static void esp_partition_iterator_release(esp_partition_iterator_t it) {--iterators;delete it;}
static esp_partition_iterator_t esp_partition_next(esp_partition_iterator_t it) {
  if(++it->index<partitions.size())return it;
  esp_partition_iterator_release(it);return nullptr;
}
static esp_err_t esp_flash_read(esp_flash_t *c,void *data,uint32_t offset,size_t size) {
  assert(c==&chip && offset>=0x3d0000 && uint64_t(offset)+size<=flash.size());
  ++reads;if(readFail || reads==readFailAt)return ESP_FAIL;
  memcpy(data,flash.data()+offset,size);return ESP_OK;
}
static esp_err_t esp_flash_write(esp_flash_t *c,const void *data,uint32_t offset,size_t size) {
  assert(c==&chip && offset==0x3d0000 && size==36);
  ++writes;
  const auto p=static_cast<const uint8_t *>(data);
  for(size_t i=0;i<(writeFail?size/2:size);++i){assert((flash[offset+i]&p[i])==p[i]);flash[offset+i]=p[i];}
  if(verifyFail)flash[offset]&=0xfe;
  return writeFail?ESP_FAIL:ESP_OK;
}
static esp_err_t nvs_flash_init_partition_ptr(const esp_partition_t *p) {
  assert(p->flash_chip==&chip && p->address==0x3d1000 && p->size==0x2f000 &&
         p->type==ESP_PARTITION_TYPE_DATA && p->subtype==ESP_PARTITION_SUBTYPE_DATA_NVS &&
         !strcmp(p->label,"iotc_hap") && !p->encrypted);
  ++inits;return initFail?ESP_FAIL:ESP_OK;
}
static esp_err_t esp_partition_erase_range(const esp_partition_t *p,size_t offset,size_t size) {
  assert(p->address==0x3d1000 && offset==0 && size==0x2f000);
  ++erases;
  std::fill_n(flash.begin()+p->address,eraseFail?size/2:size,0xff);
  durable.clear();staged.clear();
  return eraseFail?ESP_FAIL:ESP_OK;
}

static unsigned blobCopies,commits;
static bool queryFail,copyFail,setFail,commitFail,eraseKeyFail;
static const Blob *value(const char *key) {
  const auto pending=staged.find(key);if(pending!=staged.end())return &pending->second;
  const auto saved=durable.find(key);return saved==durable.end()?nullptr:&saved->second;
}
static esp_err_t nvs_get_blob(nvs_handle,const char *key,void *data,size_t *length) {
  if(!data && queryFail)return ESP_FAIL;
  const auto v=value(key);if(!v)return ESP_ERR_NVS_NOT_FOUND;
  if(!data){*length=v->size();return ESP_OK;}
  ++blobCopies;if(copyFail)return ESP_FAIL;
  assert(*length>=v->size());
  std::copy(v->begin(),v->end(),static_cast<uint8_t *>(data));*length=v->size();return ESP_OK;
}
static esp_err_t nvs_get_str(nvs_handle handle,const char *key,char *data,size_t *length) {
  return nvs_get_blob(handle,key,data,length);
}
static esp_err_t nvs_set_blob(nvs_handle,const char *key,const void *data,size_t size) {
  if(setFail)return ESP_FAIL;
  const auto p=static_cast<const uint8_t *>(data);staged[key]=Blob(p,p+size);return ESP_OK;
}
static esp_err_t nvs_commit(nvs_handle) {
  ++commits;if(commitFail)return ESP_FAIL;
  for(const auto &p:staged){if(p.second.empty())durable.erase(p.first);else durable[p.first]=p.second;}
  staged.clear();return ESP_OK;
}
static esp_err_t nvs_erase_key(nvs_handle,const char *key) {
  if(eraseKeyFail)return ESP_FAIL;
  if(!value(key))return ESP_ERR_NVS_NOT_FOUND;
  staged[key]={};return ESP_OK;
}
#include "hub_home_storage.h"
#include "../../lib/HomeSpan/src/IoTCloudNVS.h"
using iotcloud::HomeNVS;
using iotcloud::HomeStorage;
static void resetModel(bool stock=false) {
  assert(!iterators);
  flash.fill(0xff);std::fill(flash.begin(),flash.begin()+HomeStorage::Start,0x5a);
  partitions.assign(2,{});
  partitions[0].flash_chip=partitions[1].flash_chip=&chip;
  partitions[0].type=partitions[1].type=ESP_PARTITION_TYPE_APP;
  partitions[0].subtype=ESP_PARTITION_SUBTYPE_APP_OTA_0;
  partitions[1].subtype=ESP_PARTITION_SUBTYPE_APP_OTA_1;
  partitions[0].address=0x10000;partitions[1].address=stock?0x190000:0x1f0000;
  partitions[0].size=partitions[1].size=stock?0x180000:0x1e0000;
  esp_partition_t data;
  data.flash_chip=&chip;data.type=ESP_PARTITION_TYPE_DATA;
  data.subtype=ESP_PARTITION_SUBTYPE_DATA_NVS;data.address=0x9000;data.size=stock?0x4000:0x5000;
  partitions.push_back(data);
  data.subtype=ESP_PARTITION_SUBTYPE_DATA_OTA;data.address=stock?0xd000:0xe000;data.size=0x2000;
  partitions.push_back(data);
  if(stock){data.subtype=ESP_PARTITION_SUBTYPE_DATA_PHY;data.address=0xf000;data.size=0x1000;partitions.push_back(data);}
  esp_flash_default_chip=&chip;
  flashSize=flash.size();reads=writes=erases=inits=0;
  finds=readFailAt=findFailAt=0;
  readFail=writeFail=verifyFail=eraseFail=initFail=sizeFail=false;
  durable.clear();staged.clear();blobCopies=commits=0;
  queryFail=copyFail=setFail=commitFail=eraseKeyFail=false;
}
static void noClaim() {
  HomeStorage s;assert(!s.open(true)&&!s.initialized&&!s.owned);
  assert(!writes&&!erases&&!inits&&!iterators);
  assert(flash[HomeStorage::Start-1]==0x5a);
  assert(!s.reset()&&!erases&&!s.owned&&!iterators);
}
static void layoutTests(bool stock) {
  resetModel(stock);HomeStorage blank;assert(!blank.open(false)&&!writes&&!erases&&!inits);
  assert(blank.open(true)&&blank.initialized&&writes==1&&inits==1&&!erases);
  assert(blank.open(true)&&writes==1&&inits==1);
  const auto marker=Blob(flash.begin()+HomeStorage::Start,flash.begin()+HomeStorage::Start+0x1000);
  const uint8_t paired[]={1,2,3,4};uint8_t restored[4]={};
  HomeNVS pairedStore;pairedStore.opened();assert(pairedStore.writeBlob(1,"ACCESSORY",paired,sizeof(paired)));
  staged.clear(); // Power loss discards only uncommitted NVS writes.
  HomeStorage reboot;assert(reboot.open(false)&&writes==1&&inits==2&&!erases);
  HomeNVS rebootStore;rebootStore.opened();
  assert(rebootStore.blob(1,"ACCESSORY",restored,sizeof(restored))==HomeNVS::Found&&!memcmp(restored,paired,sizeof(paired)));
  flash[0x3d1000]=0x42;
  assert(reboot.reset()&&erases==1&&!reboot.initialized&&!reboot.open(true));
  assert(std::equal(marker.begin(),marker.end(),flash.begin()+HomeStorage::Start));
  assert(flash[0x3cffff]==0x5a&&flash.back()==0xff&&flash[0x3d1000]==0xff);
  HomeStorage resetBoot;assert(resetBoot.open(false)&&durable.empty());
  // Entry order is not part of either layout: the exact stock table lists
  // NVS/OTA-data/PHY before its two application slots.
  resetModel(stock);std::rotate(partitions.begin(),partitions.begin()+2,partitions.end());
  HomeStorage reordered;assert(reordered.open(true)&&reordered.owned&&!iterators);

  for(auto offset:{HomeStorage::Start,HomeStorage::Start+35,HomeStorage::Start+0x1000,0x3fffffu}){
    resetModel(stock);flash[offset]=0x17;noClaim();
    HomeStorage unknown;assert(!unknown.reset()&&!erases);
  }
  for(uint32_t size:{0x200000u,0x800000u}){resetModel(stock);flashSize=size;noClaim();}
  resetModel(stock);sizeFail=true;noClaim();
  resetModel(stock);esp_flash_default_chip=nullptr;noClaim();
  resetModel(stock);partitions.erase(partitions.begin()+1);noClaim();
  // Nearby addresses/sizes and mixed profiles never become a third layout.
  for(unsigned slot=0;slot<2;++slot)for(int delta:{-0x1000,0x1000}){
    resetModel(stock);partitions[slot].address+=delta;noClaim();assert(!reads);
    resetModel(stock);partitions[slot].size+=delta;noClaim();assert(!reads);
  }
  resetModel(stock);partitions[1].address=stock?0x1f0000:0x190000;noClaim();
  resetModel(stock);partitions[0].size=stock?0x1e0000:0x180000;noClaim();
  resetModel(stock);partitions[0].size=partitions[1].size=0x100000;partitions[1].address=0x110000;noClaim();
  for(unsigned entry=0;entry<partitions.size();++entry){
    resetModel(stock);partitions[entry].encrypted=true;noClaim();assert(!reads);
    resetModel(stock);partitions[entry].flash_chip=&otherChip;noClaim();assert(!reads);
  }
  resetModel(stock);partitions[0].flash_chip=partitions[1].flash_chip=&otherChip;noClaim();
  // Whole-table checks reject overlaps between data partitions or an app,
  // duplicate slots, external chips, the marker sector and all NVS bytes.
  for(const auto &bounds:std::vector<std::pair<uint32_t,uint32_t>>{
      {0x9000,0x1000},{0xf000,0x2000},{0x10000,0x1000},
      {0x3d0000,0x1000},{0x3d1000,0x1000},{0x3ff000,0x1000},
      {0x3c0000,0x10001},{0x3f0000,0x20000},{0xfffffff0,0x100},
      {0,0x1000},{0x300001,0x1000},{0x300000,0},{0x300000,1}}){
    resetModel(stock);esp_partition_t extra;
    extra.flash_chip=&chip;extra.type=ESP_PARTITION_TYPE_DATA;
    extra.address=bounds.first;extra.size=bounds.second;partitions.push_back(extra);noClaim();assert(!reads);
  }
  resetModel(stock);partitions.push_back(partitions[0]);noClaim();
  resetModel(stock);std::swap(partitions[2].address,partitions[3].address);noClaim();
  if(stock){
    resetModel(stock);esp_partition_t third=partitions[0];third.address=0x310000;third.size=0x10000;
    third.subtype=ESP_PARTITION_SUBTYPE_APP_OTA_1+1;partitions.push_back(third);noClaim();
  }
  for(unsigned allocation:{1u,2u,4u}){
    resetModel(stock);findFailAt=allocation;HomeStorage unavailable;
    assert(!unavailable.open(true)&&!unavailable.initialized&&!unavailable.owned);
    assert(!reads&&!writes&&!erases&&!inits&&!iterators);
    assert(!unavailable.reset()&&!writes&&!erases&&!inits&&!iterators);
  }
  resetModel(stock);readFail=true;noClaim();
  // Fail in the middle and at the end of the all-FF scan, then during marker
  // read-back. No failure permits NVS initialization or erasing unknown data.
  for(unsigned failure:{2u,99u,770u}){
    resetModel(stock);readFailAt=failure;HomeStorage unreadable;
    assert(!unreadable.open(true)&&!unreadable.initialized&&!inits&&!erases);
    assert(writes==(failure==770?1u:0u));
  }
  resetModel(stock);writeFail=true;HomeStorage interrupted;assert(!interrupted.open(true)&&writes==1&&!erases&&!inits);
  writeFail=false;HomeStorage partial;assert(!partial.open(true)&&writes==1&&!erases&&!inits);
  resetModel(stock);verifyFail=true;HomeStorage mismatch;assert(!mismatch.open(true)&&writes==1&&!erases&&!inits&&!mismatch.owned);
  verifyFail=false;assert(!mismatch.reset()&&!erases);
  resetModel(stock);initFail=true;HomeStorage broken;assert(!broken.open(true)&&writes==1&&!erases&&broken.owned);
  // An authenticated reset can recover recognized but corrupt NVS. Unknown
  // data never receives ownership merely because initialization failed.
  flash[0x3d1000]=0;assert(broken.reset()&&erases==1);
  initFail=false;HomeStorage recovered;assert(recovered.open(false));
  resetModel(stock);HomeStorage extraMarker;assert(extraMarker.open(true));
  flash[HomeStorage::Start+100]=0;HomeStorage foreign;assert(!foreign.open(true)&&writes==1&&!erases);
  assert(!foreign.reset()&&!erases);
  assert(!extraMarker.reset()&&!erases);
  resetModel(stock);initFail=true;HomeStorage stale;assert(!stale.open(true)&&stale.owned);
  flash[HomeStorage::Start]=0;assert(!stale.reset()&&!erases);
  resetModel(stock);HomeStorage tornErase;assert(tornErase.open(true));eraseFail=true;
  assert(!tornErase.reset()&&!tornErase.initialized&&!tornErase.open(true));
  assert(!tornErase.reset()&&erases==1&&!iterators);
}

int main() {
  layoutTests(false);layoutTests(true);
  const uint8_t old[]={1,2,3,4},next[]={5,6,7,8};uint8_t buffer[4]={};
  resetModel();HomeNVS nvs;nvs.opened();
  assert(nvs.blob(1,"optional",buffer,4,true)==HomeNVS::Missing&&nvs.healthy());
  assert(nvs.writeBlob(1,"ACCESSORY",old,4)&&durable["ACCESSORY"]==Blob(old,old+4));
  assert(nvs.blob(1,"ACCESSORY",buffer,4)==HomeNVS::Found&&!memcmp(old,buffer,4));
  commitFail=true;
  assert(!nvs.writeBlob(1,"ACCESSORY",next,4)&&!nvs.healthy());
  // NVS may read staged bytes after a failed commit. The health latch still
  // rejects pairing success, and a power cycle restores the committed bytes.
  assert(value("ACCESSORY")->at(0)==5&&durable["ACCESSORY"][0]==1);
  assert(!nvs.writeBlob(1,"another",next,4));
  staged.clear();commitFail=false;HomeNVS powerCycle;powerCycle.opened();
  assert(powerCycle.blob(1,"ACCESSORY",buffer,4)==HomeNVS::Found&&!memcmp(old,buffer,4));
  assert(powerCycle.erase(1,"ACCESSORY")&&!durable.count("ACCESSORY")&&commits==3);
  HomeNVS removedBoot;removedBoot.opened();assert(removedBoot.blob(1,"ACCESSORY",buffer,4,true)==HomeNVS::Missing);
  assert(removedBoot.erase(1,"already-absent")&&removedBoot.healthy());
  for(size_t size:{0u,1u,3u,5u,65536u}){
    resetModel();durable["bad"]=Blob(size,0);HomeNVS invalid;invalid.opened();
    assert(invalid.blob(1,"bad",buffer,4,true)==HomeNVS::Error&&!invalid.healthy()&&!blobCopies);
  }
  resetModel();HomeNVS missing;missing.opened();assert(missing.blob(1,"VERIFYDATA",buffer,4)==HomeNVS::Error&&!missing.healthy());
  resetModel();HomeNVS query;query.opened();queryFail=true;
  assert(query.blob(1,"bad",buffer,4,true)==HomeNVS::Error&&!query.healthy());
  resetModel();durable["ACCESSORY"]=Blob(old,old+4);HomeNVS read;read.opened();copyFail=true;
  assert(read.blob(1,"ACCESSORY",buffer,4)==HomeNVS::Error&&!read.healthy());
  resetModel();HomeNVS set;set.opened();setFail=true;
  assert(!set.writeBlob(1,"ACCESSORY",old,4)&&!set.healthy()&&!commits);
  resetModel();HomeNVS erase;erase.opened();eraseKeyFail=true;
  assert(!erase.erase(1,"CONTROLLERS")&&!erase.healthy());
  resetModel();HomeNVS controllerLength;controllerLength.opened();durable["CONTROLLERS"]=Blob(71,0);size_t length;
  assert(controllerLength.blobLength(1,"CONTROLLERS",length,70,70*16,70,true)==HomeNVS::Error&&!blobCopies);
  for(auto text:std::vector<Blob>{{'a',0},{'a',0,'b',0},{'a','b'},Blob(20,'x')}){
    resetModel();durable["SETUPID"]=text;HomeNVS string;string.opened();char setup[5]={};
    assert((string.text(1,"SETUPID",setup,sizeof(setup))==HomeNVS::Found)==(text==Blob({'a',0})));
  }
  assert(!iterators);puts("hub HAP Flash ownership and NVS durability tests passed");
}
