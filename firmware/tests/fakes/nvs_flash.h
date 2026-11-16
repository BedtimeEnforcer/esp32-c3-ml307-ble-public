#pragma once
constexpr int ESP_OK=0;
inline int fakeNvsPartitionInitResult=0;
inline int nvs_flash_init_partition(const char*){return fakeNvsPartitionInitResult;}
struct nvs_stats_t {size_t free_entries=0;};
inline int nvs_get_stats(const char* label,nvs_stats_t* stats){stats->free_entries=label&&std::string(label)=="smsdata"?2048:128;return ESP_OK;}
