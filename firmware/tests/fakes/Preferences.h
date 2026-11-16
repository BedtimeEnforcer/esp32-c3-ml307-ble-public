#pragma once
#include "Arduino.h"
#include <map>
inline std::map<std::string,std::string> fakeNvs;
inline bool fakeNvsCorruptWrite=false;
inline bool fakeNvsFailWrite=false;
enum PreferenceType { PT_STR,PT_BLOB,PT_INVALID };
inline std::map<std::string,PreferenceType> fakeNvsTypes;
inline std::map<std::string,std::string> fakeExtraNvs;
inline std::map<std::string,PreferenceType> fakeExtraNvsTypes;
inline int fakeNvsWriteBudget=-1;
class Preferences {
  bool extra_=false;
  auto& data(){return extra_?fakeExtraNvs:fakeNvs;}
  auto& types(){return extra_?fakeExtraNvsTypes:fakeNvsTypes;}
  bool canWrite(){if(fakeNvsFailWrite || fakeNvsWriteBudget==0)return false;if(fakeNvsWriteBudget>0)--fakeNvsWriteBudget;return true;}
 public:
  bool begin(const char*,bool,const char* partition=nullptr){extra_=partition && std::string(partition)=="smsdata";return true;}
  void end(){extra_=false;}
  PreferenceType getType(const char* key){return data().count(key)?types().count(key)?types()[key]:PT_STR:PT_INVALID;}
  String getString(const char* key){return getType(key)==PT_STR?String(data()[key]):String();}
  size_t getBytesLength(const char* key){return getType(key)==PT_BLOB?data()[key].size():0;}
  size_t getBytes(const char* key,void* dest,size_t size){auto n=getBytesLength(key);if(size<n)return 0;memcpy(dest,data()[key].data(),n);return n;}
  size_t putBytes(const char* key,const void* bytes,size_t size){if(!canWrite())return 0;types()[key]=PT_BLOB;data()[key]=fakeNvsCorruptWrite?"broken":std::string(static_cast<const char*>(bytes),size);return size;}
  size_t putString(const char* key,const String& s){if(!canWrite()||s.length()>=4000)return 0;types()[key]=PT_STR;data()[key]=fakeNvsCorruptWrite?"broken":s.value;return s.length();}
  bool remove(const char* key){types().erase(key);return data().erase(key)!=0;}
  size_t freeEntries(){return extra_?2048:128;}
  bool clear(){data().clear();types().clear();return true;}
};
