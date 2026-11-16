#pragma once
#include "core.h"
#include <ArduinoJson.h>

namespace bt {
struct WorkSlot {uint16_t start=480,end=495;};
struct Config {
  uint32_t schema = 1, generation = 1;
  uint32_t revision = 1;
  uint32_t wifiGeneration=1,mqttGeneration=1;
  std::string appliedCommand;
  std::vector<WorkSlot> slots{{480,495},{1225,1235}};
  std::string deviceId, masterId = "master-01", prefix = "", targetMac;
  std::string wifiSsid, wifiPass, broker, mqttUser, mqttPass, caPem;
  std::string apPass, adminPass, apn, modemCa = "btclone-ca";
  bool tls = true, labPlaintext = false, fixed = false, sleep = false, batteryMode = false;
  bool strict = true, simulate = true;
  bool displayLock = false, recoveryRestart = false;
  uint32_t retryAttempts = 3, retryBaseSeconds = 10, retryMaxSeconds = 60;
  uint32_t cooldownSeconds = 0, connectSeconds = 30, restartBatches = 3;
  int port = 8883, rollOffset = 25, rollLength = 4, rollMirror = 42;
  uint32_t scanEvery = 86400, scanSeconds = 20, uploadEvery = 86400, workSeconds = 180;
  uint32_t apSeconds = 300, screenSeconds = 30, staleSeconds = 1209600;
  uint32_t maintenanceBootSeconds = 120;
  double usableMah = 1800, sleepMa = 5, activeMa = 60, modemMa = 200;
  Frame fixedFrame;
};
bool configValid(const Config& c, bool master, std::string& error);
void configJson(const Config& c, JsonDocument& doc, bool secrets);
bool configPatch(Config& c, JsonObjectConst input, bool master, std::string& error);
bool advanceNetworkConfig(const Config& before,Config& after,std::string& error);
bool decodeMessage(const std::string& json, Message& value, std::string& error);
std::string encodeMessage(const Message& value, int batteryPercent);
void messageJson(const Message& value, JsonDocument& doc, int batteryPercent);
std::string topic(const Config& c, const char* leaf);
}
