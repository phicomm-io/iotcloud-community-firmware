#ifdef IOTCLOUD_BRY88_SCREEN
#include <esp_partition.h>
// The Arduino bootstrap may try to format NVS on a version/full error. Keep
// factory Wi-Fi, identity and certificate namespaces during this migration.
extern "C" esp_err_t __real_esp_partition_erase_range(const esp_partition_t *,size_t,size_t);
extern "C" esp_err_t __wrap_esp_partition_erase_range(const esp_partition_t *p,size_t offset,size_t size){
 if(p&&p->type==ESP_PARTITION_TYPE_DATA&&p->subtype==ESP_PARTITION_SUBTYPE_DATA_NVS&&offset==0&&size==p->size)return ESP_ERR_NOT_SUPPORTED;
 return __real_esp_partition_erase_range(p,offset,size);
}
#endif
