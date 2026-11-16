#pragma once
#include <stddef.h>
constexpr int ESP_PARTITION_TYPE_DATA=1,ESP_PARTITION_SUBTYPE_DATA_NVS=2;
struct esp_partition_t {size_t size=655360;};
inline esp_partition_t fakeExtraPartition;
inline bool fakeExtraPartitionAvailable=false;
inline const esp_partition_t* esp_partition_find_first(int,int,const char*) {return fakeExtraPartitionAvailable?&fakeExtraPartition:nullptr;}
