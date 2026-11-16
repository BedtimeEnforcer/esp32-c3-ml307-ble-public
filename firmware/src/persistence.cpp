#include "persistence.h"
#include <esp_partition.h>
#include <nvs_flash.h>
static String readRecord(Preferences& prefs,const char* key) {
  if(prefs.getType(key)!=PT_BLOB)return prefs.getType(key)==PT_STR?prefs.getString(key):String();
  size_t size=prefs.getBytesLength(key);if(!size || size>12000)return String();
  std::string bytes(size,'\0');
  return prefs.getBytes(key,&bytes[0],size)==size?String(bytes.c_str()):String();
}
static bool unwrap(const String& text, uint32_t& revision, std::string& payload) {
  if(text.length()>12000)return false;
  JsonDocument d; if(deserializeJson(d,text.c_str()))return false;
  if(!d["revision"].is<uint32_t>() || !d["crc"].is<uint32_t>() || !d["payload"].is<std::string>())return false;
  payload=d["payload"].as<std::string>();revision=d["revision"].as<uint32_t>();
  return revision && d["crc"].as<uint32_t>()==bt::checksum(payload);
}
static Persistence::ReadResult inspectRecord(Preferences& prefs,const char* name,std::string& value) {
  uint32_t best=0;bool exists=false;
  for(int slot=0;slot<2;++slot) {String key=String(name)+slot; uint32_t rev=0;std::string p;
    exists=exists || prefs.getType(key.c_str())!=PT_INVALID;
    if(unwrap(readRecord(prefs,key.c_str()),rev,p) && rev>best) {best=rev;value=p;}}
  return best?Persistence::ReadResult::Valid:exists?Persistence::ReadResult::Corrupt:Persistence::ReadResult::Missing;
}
static bool removeProjectRecords(Preferences& prefs) {
  for(auto name:{"config","runtime","snapshot","remote"})for(int slot=0;slot<2;++slot) {
    String key=String(name)+slot;
    if(prefs.getType(key.c_str())!=PT_INVALID && !prefs.remove(key.c_str()))return false;
  }
  return true;
}
bool Persistence::begin() {
  prefs_.end();backend_="nvs";writable_=migrated_=expectConfig_=false;error_.clear();
  auto fail=[&](const std::string& why){error_=why;writable_=false;return false;};
  const auto* extra=ROLE_MASTER?esp_partition_find_first(ESP_PARTITION_TYPE_DATA,ESP_PARTITION_SUBTYPE_DATA_NVS,"smsdata"):nullptr;
  if(!extra) {
    writable_=prefs_.begin(ROLE_MASTER?"btmaster":"btslave",false);
    if(!writable_)error_="默认存储无法打开";
    return writable_;
  }
  // The measured factory table already supplies this data/NVS partition.
  // Never resize partitions, erase it, or format on initialization failure.
  if(extra->size<65536)return fail("扩展存储分区过小，未修改");
  int result=nvs_flash_init_partition("smsdata");
  if(result!=ESP_OK)return fail("扩展存储初始化失败（"+std::to_string(result)+"），未格式化");
  backend_="smsdata";
  if(!prefs_.begin("btclone-main",false,"smsdata"))return fail("项目扩展命名空间无法打开");
  const std::string marker=prefs_.getString("_btcstate").c_str();
  writable_=true;
  if(marker=="ready") {expectConfig_=true;return true;}
  if(marker=="reset") {if(!clear())return fail("恢复出厂清理未完成");return true;}
  if(marker=="fresh") {
    std::string value;
    if(inspectRecord(prefs_,"config",value)==ReadResult::Valid) {
      if(prefs_.putString("_btcstate","ready")!=5 || prefs_.getString("_btcstate")!="ready")return fail("存储就绪标记保存失败");
      expectConfig_=true;
    }
    return true;
  }
  if(!marker.empty() && marker!="migrating")return fail("存储迁移标记异常，保留原记录");
  if(marker.empty())for(auto name:{"config","runtime","snapshot","remote"}) {
    std::string value;if(inspectRecord(prefs_,name,value)!=ReadResult::Missing)return fail("扩展存储有未确认记录，停止自动迁移");
  }
  Preferences legacy;
  bool source=legacy.begin("btmaster",true);
  std::string value;auto config=source?inspectRecord(legacy,"config",value):ReadResult::Missing;
  if(config==ReadResult::Missing && marker.empty()) {
    // A first-time board starts in the larger store. A reset/fresh marker
    // prevents an old default-NVS copy from reappearing after factory reset.
    if(prefs_.putString("_btcstate","fresh")!=5 || prefs_.getString("_btcstate")!="fresh")return fail("首次存储标记保存失败");
    return true;
  }
  if(config!=ReadResult::Valid)return fail("迁移源配置缺失或损坏，原分区保留");
  if(prefs_.putString("_btcstate","migrating")!=9 || prefs_.getString("_btcstate")!="migrating")return fail("迁移开始标记保存失败");
  for(auto name:{"snapshot","runtime","remote","config"}) {
    value.clear();auto state=inspectRecord(legacy,name,value);
    if(state==ReadResult::Corrupt)return fail(std::string("迁移源记录校验失败: ")+name);
    if(state==ReadResult::Valid && !write(name,value))return fail(std::string("迁移写入失败: ")+name+" / "+error_);
  }
  if(prefs_.putString("_btcstate","ready")!=5 || prefs_.getString("_btcstate")!="ready")return fail("迁移完成标记保存失败");
  expectConfig_=migrated_=true;return true;
}
Persistence::ReadResult Persistence::inspect(const char* name,std::string& value) {
  auto result=inspectRecord(prefs_,name,value);
  if(expectConfig_ && std::string(name)=="config" && result==ReadResult::Missing)return ReadResult::Corrupt;
  return result;
}
bool Persistence::read(const char* name,std::string& value) {return inspect(name,value)==ReadResult::Valid;}
bool Persistence::write(const char* name,const std::string& value) {
  auto fail=[&](const char* why){error_=std::string(name)+": "+why;return false;};
  if(!writable_)return fail("storage_not_ready");
  if(value.size()>9500)return fail("payload_too_large");
  uint32_t best=0;int previous=1;
  for(int slot=0;slot<2;++slot) {String key=String(name)+slot;uint32_t rev=0;std::string p;
    if(unwrap(readRecord(prefs_,key.c_str()),rev,p) && rev>best) {best=rev;previous=slot;}}
  if(best==UINT32_MAX)return fail("revision_exhausted");
  JsonDocument d;d["revision"]=best+1;d["crc"]=bt::checksum(value);d["payload"]=value;String text;serializeJson(d,text);
  // The serialized envelope owns its bytes. Release the duplicate JSON tree
  // before NVS allocation and readback while BLE, TLS and HTTP are also live.
  d.clear();
  if(text.length()>12000)return fail("record_too_large");
  String key=String(name)+(1-previous);
  // NVS strings need a contiguous run within one page, even below 4000 bytes.
  // Reclaim only the inactive slot; the latest verified slot remains intact.
  if(prefs_.getType(key.c_str())!=PT_INVALID && !prefs_.remove(key.c_str()))return fail("inactive_remove_failed");
  if(prefs_.putBytes(key.c_str(),text.c_str(),text.length())!=text.length())return fail("put_bytes_failed");
  uint32_t rev=0;std::string check;
  if(unwrap(readRecord(prefs_,key.c_str()),rev,check) && rev==best+1 && check==value) {
    if(std::string(backend_)=="smsdata" && std::string(name)=="config" && prefs_.getString("_btcstate")=="fresh") {
      if(prefs_.putString("_btcstate","ready")!=5 || prefs_.getString("_btcstate")!="ready") return fail("ready_marker_failed");
      expectConfig_=true;
    }
    error_.clear();return true;
  }
  prefs_.remove(key.c_str());return fail("readback_failed");
}
bool Persistence::clear() {
  if(std::string(backend_)=="nvs")return prefs_.clear();
  if(prefs_.putString("_btcstate","reset")!=5 || prefs_.getString("_btcstate")!="reset")return false;
  Preferences legacy;
  if(legacy.begin("btmaster",false) && !removeProjectRecords(legacy))return false;
  if(!removeProjectRecords(prefs_))return false;
  if(prefs_.putString("_btcstate","fresh")!=5 || prefs_.getString("_btcstate")!="fresh")return false;
  expectConfig_=false;return true;
}
size_t Persistence::freeEntries() {
  nvs_stats_t stats{};
  return nvs_get_stats(backend_,&stats)==ESP_OK?stats.free_entries:0;
}
