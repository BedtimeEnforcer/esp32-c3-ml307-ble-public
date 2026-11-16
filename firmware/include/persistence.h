#pragma once
#include <Preferences.h>
#include "protocol.h"

class Persistence {
  Preferences prefs_;
  const char* backend_="nvs";
  bool writable_=false,migrated_=false,expectConfig_=false;
  std::string error_;
 public:
  enum class ReadResult { Missing, Valid, Corrupt };
  bool begin();
  ReadResult inspect(const char* name,std::string& value);
  bool read(const char* name, std::string& value);
  bool write(const char* name, const std::string& value);
  size_t freeEntries();
  bool clear();
  const char* backend() const {return backend_;}
  bool migrated() const {return migrated_;}
  const std::string& lastError() const {return error_;}
};
