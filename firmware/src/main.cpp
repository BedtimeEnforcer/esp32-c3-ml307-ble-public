#ifdef BT_RUNTIME_TEST
#include "runtime_fakes.h"
#else
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_task_wdt.h>
#include <esp_sntp.h>
#include <mbedtls/x509_crt.h>
#include "policy.h"
#include "remote.h"
#include "screen_ui.h"
#include <driver/gpio.h>
#include <sys/time.h>
#include <deque>
#include <cmath>
#include "config.h"
#include "core.h"
#include "protocol.h"
#include "persistence.h"
#include "ble_scanner.h"
#include "ble_advertiser.h"
#include "modem.h"
#include "subscriber.h"
#include "web_page.h"
#include "web_stream.h"

#endif

namespace {
constexpr bool IsMaster=ROLE_MASTER;
bt::Config cfg;
bt::Message saved;
bt::Battery battery;
Persistence store;
BleScanner scanner;
BleAdvertiser advertiser;
Modem modem;
Subscriber subscriber;
WebServer web(80);
// Wire owns the pins; U8g2 must not reconfigure them after the address probe.
U8G2_SH1106_128X64_NONAME_F_HW_I2C screen(U8G2_R0,U8X8_PIN_NONE);
std::deque<std::string> logs;
std::string csrf,scanState="未扫描",uploadState="未上传",bleError,wifiTest="未测试";
bool hasSaved=false,storageOk=false,bleOk=false,screenOk=false,apOn=false,changed=false;
bool captured=false,forceUpload=false,networkTest=false,publishPending=false,cycleRunning=false;
bool dirtyCache=false,clockValid=false,restartPending=false,simulationEnabled=true;
bool factoryReset=false,configRecovery=false;
bool restartForMaintenance=false,maintenanceExitPending=false;
uint32_t maintenanceExitAt=0;
bt::MaintenanceWindow maintenanceWindow;
bool maintenanceActive() {return IsMaster && cfg.sleep && maintenanceWindow.active(millis());}
void maintenanceActivity() {if(IsMaster && cfg.sleep)maintenanceWindow.touch(cfg.maintenanceBootSeconds,millis());}
void beginMaintenanceWindow(bt::BootKind kind) {maintenanceWindow.begin(kind,IsMaster && cfg.sleep?cfg.maintenanceBootSeconds:0,millis());}
std::string configStatus="尚未读取";
bool sleepPending=false;
bool pendingUpload=false,shutdownFault=false,shutdownEpisodeHandled=true;
bt::AccessLock displayAccess;
bt::Recovery netRecovery,wifiRecovery,timeRecovery;
uint64_t lastSyncEpoch=0,lastAutoRestart=0;
bool timeJob=false,modemTimeRecorded=false,wifiConnecting=false,ntpRunning=false,displayOff=false;
bool wasWifiConnected=false,autoRestartPending=false;
volatile bool ntpReceived=false;
volatile uint8_t wifiDisconnectReason=0;
uint32_t ntpStarted=0,mqttStarted=0,mqttHealthyAt=0,wifiHealthyAt=0,bootFaults=0;
void startAp();
void closeAp() {
  if(apOn) {web.close();WiFi.softAPdisconnect(true);WiFi.mode(WIFI_STA);apOn=false;}
}
void lockScreen() {displayAccess.relock();if(cfg.displayLock)closeAp();}
void requestMaintenanceExit() {maintenanceExitPending=true;maintenanceExitAt=millis()+500;}
void maintenanceTick() {
  if(maintenanceExitPending && bt::reached(millis(),maintenanceExitAt)) {
    maintenanceExitPending=false;maintenanceWindow.cancel();closeAp();displayAccess.relock();
  }
}

uint32_t scanStarted=0,observedMillis=0,changedMillis=0,nextScan=0,nextUpload=0;
uint32_t screenUntil=0,apIdle=0,lastScreen=0,lastBattery=0,lastPersist=0,restartAt=0;
uint32_t buttonAt=0,buttonHighAt=0,wifiAttempt=0,scanCycleAt=0,lastAdvRetry=0;
uint32_t failures=0,receivedMillis=0;
uint32_t scanEnded=0;
bool scanClockPending=false;
uint64_t lastScanEpoch=0,lastUploadEpoch=0,lastReceivedEpoch=0;
bool buttonLatched=false;
bt::RemoteState remote;
uint64_t epochNow();
uint64_t scheduledWake();
void log(const std::string& text);
struct Outgoing {std::string topic,payload;bool raw=false;};
std::deque<Outgoing> outgoing;
bool remoteOnline=false,txActive=false,rawQueued=false,closingNetwork=false,slotAborted=false,sessionFailed=false;
int txId=-1;
uint32_t txStarted=0,remoteConnectedAt=0,lastTelemetry=0,sessionStarted=0,manualStarted=0;
std::string runningCommand;
void queueRemote(bool plannedSleep=false,bool full=true);
void receiveCommand(const std::string& topic,const std::string& payload);
void attemptFailed(const std::string& reason);
bool saveRemote() {
  if(configRecovery)return false;
  std::string value;
  if(!remote.encode(value) || !store.write("remote",value)) {storageOk=false;log("远程/时段记录保存失败，停止新增操作："+store.lastError());return false;}
  return true;
}
void queuePacket(const std::string& topic,const std::string& payload,bool raw=false) {
  if(payload.size()>bt::MaxPayload) {log("远程上报超长，已拒绝");return;}
  size_t start=txActive?1:0;
  for(size_t i=start;i<outgoing.size();++i)if(outgoing[i].topic==topic) {outgoing[i]={topic,payload,raw};return;}
  if(outgoing.size()<48)outgoing.push_back({topic,payload,raw});else log("上报队列已满");
}
void commandResult(const std::string& id,const std::string& state,const std::string& reason) {
  auto* r=remote.find(id);if(!r)return;
  r->state=state;r->reason=bt::shortText(reason);r->at=epochNow();
  if(state=="done" || state=="failed")remote.record(epochNow(),state=="done",r->command.action+": "+reason);
  saveRemote();queueRemote();
}

struct SleepRecord {uint32_t magic;uint64_t elapsedUs;double used;uint32_t crc;};
RTC_DATA_ATTR SleepRecord slept{};
constexpr uint32_t MaintenanceBootMagic=0x4D41494E;
// .rtc.data is reloaded by the bootloader on a software reset. Only the
// no-init section preserves this one-shot intent across ESP.restart().
RTC_NOINIT_ATTR uint32_t nextBootMaintenance;
uint64_t epochNow() {
#ifdef BT_RUNTIME_TEST
  return clockValid?fakeEpoch:0;
#else
  time_t t=time(nullptr);return clockValid && t>=1704067200 ? static_cast<uint64_t>(t):0;
#endif
}
uint32_t since(uint32_t t) {return millis()-t;}
bool due(uint32_t deadline) {return static_cast<int32_t>(millis()-deadline)>=0;}
void log(const std::string& text) {if(logs.size()>=24)logs.pop_front();logs.push_back(text);Serial.println(text.c_str());}
std::string randomText(size_t count=16) {const char* h="0123456789abcdef";std::string s;for(size_t i=0;i<count;++i)s+=h[esp_random()&15];return s;}
std::string jsonString(JsonDocument& d) {std::string out;serializeJson(d,out);return out;}
bool loadConfiguration() {
  if(!storageOk) {configRecovery=true;configStatus="存储无法打开，请本地维护："+store.lastError();return false;}
  std::string text,error;auto result=store.inspect("config",text);
  if(result==Persistence::ReadResult::Valid) {
    JsonDocument d;
    if(!deserializeJson(d,text) && d["schema"]==1 && d["revision"].is<uint32_t>()) {
      if(!IsMaster)d["sleep"]=false;
      if(bt::configPatch(cfg,d.as<JsonObjectConst>(),IsMaster,error)) {configStatus="已加载有效配置";return true;}
    }
  } else if(result==Persistence::ReadResult::Missing) {
    configStatus="未找到配置记录，首次初始化；可从备份恢复";
    JsonDocument d;bt::configJson(cfg,d,true);storageOk=store.write("config",jsonString(d));
    if(!storageOk)configStatus="首次配置保存失败";
    return false;
  }
  configRecovery=true;storageOk=false;
  configStatus="配置损坏或校验失败，原记录保留；请恢复备份或明确重置";
  log(configStatus);return false;
}
void configBackup(JsonDocument& d) {
  d["format"]="btclone-config-v1";d["role"]=IsMaster?"master":"slave";
  d["storageHealthy"]=storageOk;
  auto value=d["config"].to<JsonObject>();JsonDocument full;bt::configJson(cfg,full,true);value.set(full.as<JsonObjectConst>());
}
std::string stamp(uint64_t epoch) {
  if(!epoch)return "时间未知";
  time_t t=epoch;tm local{};localtime_r(&t,&local);char text[32];strftime(text,sizeof(text),"%m-%d %H:%M:%S",&local);return text;
}
#ifndef BT_RUNTIME_TEST
void sendJson(int code,JsonDocument& d) {web.sendHeader("Cache-Control","no-store");web.send(code,"application/json; charset=utf-8",jsonString(d).c_str());}
void reply(int code,const std::string& message) {JsonDocument d;d[code>=400?"error":"message"]=message;sendJson(code,d);}
bool auth(bool mutation=false) {
  if(displayAccess.locked() || !apOn) {web.send(404,"text/plain","");return false;}
  if(!web.authenticate("admin",cfg.adminPass.c_str())) {web.requestAuthentication();return false;}
  if(mutation && web.header("X-CSRF-Token")!=csrf.c_str()) {reply(403,"页面令牌失效，请刷新后重试");return false;}
  return true;
}
bool body(JsonDocument& d) {
  String raw=web.arg("plain");
  if(raw.length()>9500 || deserializeJson(d,raw,DeserializationOption::NestingLimit(4)) || !d.is<JsonObject>()) {reply(400,"JSON 无效或超出9500字节");return false;}
  return true;
}
#endif
bool saveRuntime() {
  if(configRecovery)return false;
  JsonDocument d;d["used"]=battery.usedMah;d["calibrated"]=battery.calibrated;
  d["lastScan"]=lastScanEpoch;d["lastUpload"]=lastUploadEpoch;d["master"]=cfg.masterId;d["generation"]=cfg.generation;
  d["mac"]=cfg.targetMac;d["failures"]=failures;
  d["sleepPending"]=sleepPending;
  d["pendingUpload"]=pendingUpload;d["shutdownFault"]=shutdownFault;
  d["lastSync"]=lastSyncEpoch;d["lastAutoRestart"]=lastAutoRestart;d["bootFaults"]=bootFaults;
  d["wifiGeneration"]=cfg.wifiGeneration;d["mqttGeneration"]=cfg.mqttGeneration;
  auto recovery=[&](const char* key,const bt::Recovery& r) {
    auto v=d[key].to<JsonObject>();v["attempts"]=r.attempts;v["batches"]=r.batches;
    v["retryEpoch"]=r.retryEpoch;v["waiting"]=r.waiting;
  };
  recovery("netRecovery",netRecovery);recovery("wifiRecovery",wifiRecovery);recovery("timeRecovery",timeRecovery);
  bool ok=store.write("runtime",jsonString(d));lastPersist=millis();if(!ok)log("运行记录持久化失败："+store.lastError());return ok;
}
bool saveSnapshot(const bt::Message& value) {
  if(configRecovery)return false;
  JsonDocument d;d["prefix"]=cfg.prefix;d["broker"]=cfg.broker;d["port"]=cfg.port;
  d["message"]=bt::encodeMessage(value,-1);return store.write("snapshot",jsonString(d));
}
void recoveryFailed(bt::Recovery& r,const std::string& reason) {
  bool exhausted=r.fail(cfg,millis(),epochNow());
  log(reason+"；等待"+std::to_string(r.waitSeconds)+"秒"+(exhausted?"（本批统计完成，继续短退避）":"（退避）"));
  // Preserve bounded backoff and restart rate limits across reboot.
  if(exhausted && bt::mayRestart(cfg,r.batches,epochNow(),lastAutoRestart))autoRestartPending=true;
  saveRuntime();
}
bool beginModem(bool onlyNetwork=false) {
  if(modem.busy() || !netRecovery.allowed(millis()))return false;
  if(ntpRunning) {sntp_stop();ntpRunning=false;ntpReceived=false;}
  modemTimeRecorded=false;
  shutdownEpisodeHandled=false;
  sessionFailed=false;sessionStarted=millis();return modem.start(cfg,onlyNetwork,!onlyNetwork);
}
Snapshot toSnapshot(const bt::Frame& f) {
  Snapshot s{};strncpy(s.deviceMac,f.mac.c_str(),17);s.addressType=BLE_ADDR_RANDOM;
  s.advType=f.pdu=="ADV_IND"?BLE_HCI_ADV_RPT_EVTYPE_ADV_IND:f.pdu=="ADV_SCAN_IND"?BLE_HCI_ADV_RPT_EVTYPE_SCAN_IND:BLE_HCI_ADV_RPT_EVTYPE_NONCONN_IND;
  s.advLength=f.adv.size();s.responseLength=f.rsp.size();memcpy(s.advData,f.adv.data(),f.adv.size());memcpy(s.responseData,f.rsp.data(),f.rsp.size());
  s.rawLength=s.advLength+s.responseLength;memcpy(s.rawData,s.advData,s.advLength);memcpy(s.rawData+s.advLength,s.responseData,s.responseLength);return s;
}
void startAdvertising() {
  if(IsMaster || !bleOk)return;
  advertiser.stop();bleError.clear();
  if(!simulationEnabled)return;
  if(!cfg.fixed && !hasSaved) {bleError="尚无有效缓存";return;}
  bt::Frame f=cfg.fixed?cfg.fixedFrame:saved.frame;std::string error;
  if(!bt::frameValid(f,cfg.fixed?false:cfg.strict,error)) {bleError=error;return;}
  if(!advertiser.start(toSnapshot(f)))bleError=advertiser.lastError();
  log(advertiser.active()?"从端原始广播已启动":"从端广播未启动："+bleError);
}
int saveLocalConfig(JsonDocument& d,JsonDocument& result) {
  auto fail=[&](int code,const std::string& error){result["error"]=error;return code;};
  if(restartPending)return fail(409,"设备即将重启，请等待重新连接");
  if(!d["expectedRevision"].is<uint32_t>() || d["expectedRevision"].as<uint32_t>()!=cfg.revision)return fail(409,"配置已被其他入口修改，请刷新后重试");
  d.remove("expectedRevision");
  if(!d["revision"].isNull() || !d["wifiGeneration"].isNull() || !d["mqttGeneration"].isNull() || !d["appliedCommand"].isNull() || cfg.revision==UINT32_MAX)return fail(400,"配置版本为只读");
  bt::Config next=cfg;std::string error;
  if(!bt::configPatch(next,d.as<JsonObjectConst>(),IsMaster,error) || !bt::advanceNetworkConfig(cfg,next,error))return fail(400,error);
  ++next.revision;next.appliedCommand.clear();JsonDocument stored;bt::configJson(next,stored,true);
  std::string encoded=jsonString(stored);
  // Parsing/validation is complete; retain neither request nor serialization
  // trees during the NVS transaction. The current config stays untouched.
  stored.clear();d.clear();
  if(!next.caPem.empty()) {
    mbedtls_x509_crt certificate;mbedtls_x509_crt_init(&certificate);
    int parsed=mbedtls_x509_crt_parse(&certificate,reinterpret_cast<const unsigned char*>(next.caPem.c_str()),next.caPem.size()+1);
    mbedtls_x509_crt_free(&certificate);
    if(parsed!=0)return fail(400,"CA 证书无法完整解析，旧配置保留");
  }
  if(!store.write("config",encoded)) {
    log("配置保存失败："+store.lastError());
    return fail(500,"NVS 保存失败，旧配置保留（"+store.lastError()+"）");
  }
  JsonDocument visible;bt::configJson(next,visible,false);result["config"].set(visible.as<JsonObjectConst>());
  result["message"]="参数已写入，设备即将重启，请等待回读确认";result["revision"]=next.revision;
  restartForMaintenance=true;restartPending=true;restartAt=millis()+1000;return 200;
}
int restoreLocalConfig(JsonDocument& backup,uint32_t revision,JsonDocument& result) {
  if(backup["format"]!="btclone-config-v1" || backup["role"]!=(IsMaster?"master":"slave") || !backup["config"].is<JsonObject>()) {
    result["error"]="备份格式或主从角色不匹配";return 400;
  }
  JsonDocument values;values.set(backup["config"].as<JsonObjectConst>());
  for(auto key:{"revision","wifiGeneration","mqttGeneration","appliedCommand","wifiPasswordSet","mqttPasswordSet"})values.remove(key);
  for(auto key:{"wifiPass","mqttPass"})if(values[key].is<std::string>() && values[key].as<std::string>().empty())values[key==std::string("wifiPass")?"clearWifiPass":"clearMqttPass"]=true;
  values["expectedRevision"]=revision;return saveLocalConfig(values,result);
}
void usbRequest(const std::string& text,JsonDocument& reply) {
  static uint32_t deniedAt=0;static unsigned denied=0;
  JsonDocument request;
  if(text.size()>9500 || deserializeJson(request,text,DeserializationOption::NestingLimit(5)) || !request.is<JsonObject>()) {reply["error"]="invalid_request";return;}
  if(denied>=3 && since(deniedAt)<300000) {reply["error"]="authentication_cooldown";return;}
  if(!request["password"].is<std::string>() || request["password"].as<std::string>()!=cfg.adminPass) {
    deniedAt=millis();denied=std::min(denied+1,3U);reply["error"]="authentication_failed";return;
  }
  denied=0;
  if(request["op"]=="diagnostic") {
    reply["ok"]=true;reply["revision"]=cfg.revision;reply["firmwareVersion"]="0.3.7-dev+r1";reply["usbProtocol"]=2;
    reply["configRecovery"]=configRecovery;reply["configStatus"]=configStatus;
    reply["storageBackend"]=store.backend();reply["storageError"]=store.lastError();reply["storageMigrated"]=store.migrated();
    reply["screenDetected"]=screenOk;reply["cellular"]=IsMaster && modem.registered;
    if(IsMaster) {reply["modemStatus"]=modem.status;reply["simStatus"]=modem.simStatus;reply["modemFailureStep"]=modem.failureStep;reply["modemErrorCode"]=modem.errorCode;reply["modemConnectionCode"]=modem.connectionCode;}
    if(IsMaster) {reply["modemRxBytes"]=modem.receivedBytes;reply["modemPowerKeyHigh"]=digitalRead(5)==HIGH;}
    if(IsMaster) {reply["modemReadShape"]=modem.lastReadShape;reply["modemReadFailureShape"]=modem.lastReadFailureShape;reply["modemEmptyReadShape"]=modem.lastEmptyReadShape;reply["modemReadFailures"]=modem.readFailures;}
    reply["wifi"]=WiFi.status()==WL_CONNECTED;reply["wifiDisconnectReason"]=wifiDisconnectReason;
    reply["wifiIp"]=WiFi.localIP().toString();reply["apClients"]=apOn?WiFi.softAPgetStationNum():0;
    reply["maintenanceWindowActive"]=maintenanceActive();reply["maintenanceWindowSeconds"]=maintenanceActive()?maintenanceWindow.remaining(millis()):0;
    if(!IsMaster) {
      reply["mqttErrors"]=int(subscriber.errors);reply["mqttErrorType"]=int(subscriber.lastErrorType);
      reply["mqttTlsError"]=int(subscriber.tlsError);reply["mqttTlsStackError"]=int(subscriber.tlsStackError);
      reply["mqttCertFlags"]=int(subscriber.tlsCertFlags);reply["mqttSocketError"]=int(subscriber.socketError);reply["mqttBrokerCode"]=int(subscriber.brokerCode);
    }
    reply["mqtt"]=IsMaster?modem.connected:subscriber.connected;reply["subscribed"]=!IsMaster && subscriber.subscribed;
    reply["wifiSet"]=!cfg.wifiSsid.empty();reply["brokerSet"]=!cfg.broker.empty();reply["mqttUserSet"]=!cfg.mqttUser.empty();reply["caSet"]=!cfg.caPem.empty();
    reply["freeNvsEntries"]=store.freeEntries();reply["timeValid"]=epochNow()!=0;reply["storageOk"]=storageOk;reply["ap"]=apOn;return;
  }
  if(request["op"]=="backup") {
    if(configRecovery) {reply["error"]="config_unavailable";return;}
    reply["ok"]=true;configBackup(reply);return;
  }
  if(request["op"]=="maintenance") {
    if(displayAccess.locked()) {reply["error"]="display_locked";return;}
    startAp();reply["ok"]=apOn;return;
  }
  if(request["op"]=="restore" && request["values"].is<JsonObject>()) {
    if(!request["expectedRevision"].is<uint32_t>()) {reply["error"]="missing_revision";return;}
    JsonDocument backup,result;backup.set(request["values"].as<JsonObjectConst>());
    int code=restoreLocalConfig(backup,request["expectedRevision"].as<uint32_t>(),result);
    reply["ok"]=code==200;reply["status"]=code;
    if(code==200)reply["revision"]=result["revision"];else reply["error"]=result["error"];
    return;
  }
  if(request["op"]!="config" || !request["values"].is<JsonObject>()) {reply["error"]="unsupported_operation";return;}
  JsonDocument values;values.set(request["values"].as<JsonObjectConst>());values["expectedRevision"]=request["expectedRevision"];
  JsonDocument result;int code=saveLocalConfig(values,result);
  // The USB acknowledgement never exports credentials, raw data, identifiers or config values.
  reply["ok"]=code==200;reply["status"]=code;
  if(code==200)reply["revision"]=result["revision"];else reply["error"]=result["error"];
}
void usbFrame(const std::string& line,JsonDocument& result) {
  if(line.compare(0,9,"BTLOCAL2 ")==0) {
    size_t a=line.find(' ',9),b=a==std::string::npos?a:line.find(' ',a+1);
    auto number=[&](size_t start,size_t end,uint64_t limit,uint64_t& value) {
      value=0;if(end==std::string::npos || start==end || end-start>10)return false;
      for(size_t i=start;i<end;++i) {if(line[i]<'0' || line[i]>'9')return false;value=value*10+(line[i]-'0');if(value>limit)return false;}
      return true;
    };
    uint64_t size=0,crc=0;
    if(!number(9,a,9500,size) || a==std::string::npos || !number(a+1,b,UINT32_MAX,crc) || b==std::string::npos) {result["error"]="transport_integrity_failed";return;}
    std::string payload=line.substr(b+1);
    if(payload.size()!=size || bt::checksum(payload)!=crc) {result["error"]="transport_integrity_failed";return;}
    usbRequest(payload,result);return;
  }
  if(line.compare(0,8,"BTLOCAL ")==0) {
    JsonDocument legacy;
    if(deserializeJson(legacy,line.substr(8)) || (legacy["op"]!="diagnostic" && legacy["op"]!="backup")) {result["error"]="usb_v2_required";return;}
    usbRequest(line.substr(8),result);return;
  }
  result["error"]="invalid_protocol";
}
#ifndef BT_RUNTIME_TEST
void usbTick() {
  static std::string line;static uint32_t last=0;static bool dropping=false;
  if((!line.empty() || dropping) && since(last)>5000) {line.clear();dropping=false;}
  for(unsigned budget=0;budget<128 && Serial.available();++budget) {
    char c=Serial.read();last=millis();if(c=='\r')continue;
    if(c=='\n') {
      if(!dropping && (line.compare(0,8,"BTLOCAL ")==0 || line.compare(0,9,"BTLOCAL2 ")==0)) {
        JsonDocument result;usbFrame(line,result);std::string encoded=jsonString(result);
        if(line.compare(0,9,"BTLOCAL2 ")==0) {
          std::string prefix="BTLOCAL2 "+std::to_string(encoded.size())+" "+std::to_string(bt::checksum(encoded))+" ";Serial.print(prefix.c_str());
        } else Serial.print("BTLOCAL ");
        Serial.println(encoded.c_str());
      }
      line.clear();dropping=false;
    } else if(!dropping) {if(line.size()<9540)line+=c;else {line.clear();dropping=true;}}
  }
}
#endif
void startAp() {
  if(displayAccess.locked())return;
  if(apOn) {web.begin();screenUntil=millis()+cfg.screenSeconds*1000UL;return;}
  WiFi.mode(WIFI_AP_STA);
  String ssid=(IsMaster?"BT-M-":"BT-S-")+String(cfg.deviceId.c_str());
  if(WiFi.softAP(ssid.c_str(),cfg.apPass.c_str(),1,false,2)) {
    // Bind after enabling the maintenance interface. Reopening the AP also
    // renews the listener; do not depend on a socket created at boot.
    web.begin();
    apOn=true;apIdle=millis();screenUntil=millis()+cfg.screenSeconds*1000UL;
    log("维护热点已开启，地址 192.168.4.1，后台用户名 admin");
  } else log("维护热点开启失败");
}
bool startScan(bool upload) {
  if(!IsMaster || !bleOk || scanner.active() || cycleRunning || rawQueued || (!modem.connected && !netRecovery.allowed(millis())) || cfg.targetMac.empty() || restartPending)return false;
  scanner.target(cfg.targetMac.c_str());captured=changed=false;forceUpload=upload;
  if(!scanner.start()) {scanState="扫描启动失败";return false;}
  scanStarted=scanCycleAt=millis();cycleRunning=true;scanState="扫描中，等待目标";
  return true; // Background scans never wake or unlock the display.
}
void processObservation(const ScanObservation& o) {
  bt::Frame f;f.mac=o.snapshot.deviceMac;for(char& c:f.mac)c=toupper(static_cast<unsigned char>(c));
  f.adv={o.snapshot.advData,o.snapshot.advData+o.snapshot.advLength};
  f.rsp={o.snapshot.responseData,o.snapshot.responseData+o.snapshot.responseLength};
  if(o.snapshot.advType==BLE_HCI_ADV_RPT_EVTYPE_ADV_IND)f.pdu="ADV_IND";
  else if(o.snapshot.advType==BLE_HCI_ADV_RPT_EVTYPE_SCAN_IND)f.pdu="ADV_SCAN_IND";
  else if(o.snapshot.advType==BLE_HCI_ADV_RPT_EVTYPE_NONCONN_IND)f.pdu="ADV_NONCONN_IND";
  else {scanState="目标 PDU 类型不支持";return;}
  std::string error;
  if(o.snapshot.addressType!=BLE_ADDR_RANDOM || !bt::frameValid(f,cfg.strict,error)) {if(!captured)scanState=error.empty()?"目标地址类型不支持":error;return;}
  bool different=!hasSaved || !(f==saved.frame);
  if(different) {saved.frame=f;saved.changed=epochNow();changedMillis=millis();changed=true;}
  saved.master=cfg.masterId;saved.generation=cfg.generation;saved.rssi=o.rssi;
  saved.observed=epochNow();saved.reported=0;saved.timeValid=false;
  // A provisional observation is never published before allocating/persisting seq.
  hasSaved=captured=dirtyCache=true;observedMillis=millis();scanState="已完整抓取目标";
}
void finishScan() {
  scanner.stop();lastScanEpoch=epochNow();nextScan=millis()+cfg.scanEvery*1000UL;
  scanEnded=millis();scanClockPending=!lastScanEpoch;
  if(!captured) {
    scanState=scanner.seen()?"找到目标但未取得可用完整快照":"未发现预设 MAC";
    attemptFailed(scanState);return;
  }
  bool report=changed || forceUpload || pendingUpload || due(nextUpload) || !lastUploadEpoch || (cfg.sleep && remote.slot);
  if(report) {
    pendingUpload=true;timeJob=networkTest=false;uploadState="等待4G上传本次采集";saveRuntime();
  } else {cycleRunning=false;saveRuntime();}
}
void refreshObservedClock() {
  uint64_t now=epochNow();
  if(captured && now) {
    if(!saved.observed)saved.observed=now-since(observedMillis)/1000;
    if(changed && !saved.changed)saved.changed=now-since(changedMillis)/1000;
  }
}
void attemptFailed(const std::string& reason) {
  scanner.stop();cycleRunning=false;captured=false;pendingUpload=false;rawQueued=false;
  scanState=uploadState=reason;failures=std::min<uint32_t>(failures+1,1000000);
  recoveryFailed(netRecovery,reason);nextScan=nextUpload=netRecovery.deadline;
  if(remote.slot) {
    remote.slotReason=reason;
    uint32_t delay=std::min<uint32_t>(cfg.retryMaxSeconds,cfg.retryBaseSeconds*(1UL<<std::min<uint32_t>(remote.attempts?remote.attempts-1:0,5)));
    remote.retryAt=remote.attempts>=cfg.retryAttempts?remote.slotEnd:std::min<uint64_t>(remote.slotEnd,epochNow()+delay);
    // Retry budget is per window. Existing shutdown/abnormal-boot backoff
    // remain stronger constraints and are never cleared by a remote command.
    saveRemote();
  } else remote.record(epochNow(),false,reason);
  if(!runningCommand.empty()) {commandResult(runningCommand,"failed",reason);runningCommand.clear();}
  saveRemote();saveRuntime();queueRemote();log(reason);
}
void retireSlot(const std::string& reason,bool success) {
  remote.finishSlot(epochNow(),success,reason);saveRemote();queueRemote();
}
void masterTick() {
  if(!storageOk) {scanner.stop();modem.shutdown();modem.tick();return;}
  uint64_t now=epochNow();
  if(cfg.sleep && remote.slot && now && now>=remote.slotEnd) {
    bool running=cycleRunning || pendingUpload;
    scanner.stop();cycleRunning=false;pendingUpload=false;rawQueued=false;captured=false;
    if(running && !runningCommand.empty()) {commandResult(runningCommand,"failed","工作时段已结束");runningCommand.clear();}
    // A late PUBACK cannot retroactively make an expired window successful.
    slotAborted=true;outgoing.clear();txActive=false;
    retireSlot(remote.slotReason.empty()?"到期未完成扫描上传":remote.slotReason,false);
    closingNetwork=true;modem.shutdown();
  }
  ScanObservation o;int limit=8;while(scanner.active() && limit-- && scanner.pop(o))processObservation(o);
  if(scanner.active() && since(scanStarted)>=cfg.scanSeconds*1000UL)finishScan();
  modem.tick();
  if(modem.clockValid && !modemTimeRecorded) {
    clockValid=true;modemTimeRecorded=true;lastSyncEpoch=epochNow();timeRecovery.success();saveRuntime();
  }
  if(networkTest || timeJob) {
    if(!modem.busy()) {bool ok=timeJob?modem.clockValid:modem.networkPassed;if(!ok)recoveryFailed(timeRecovery,modem.status);timeJob=networkTest=cycleRunning=false;}
    return;
  }
  if(remoteOnline && !modem.connected) {
    remoteOnline=false;txActive=false;outgoing.clear();
    if(!closingNetwork && !sessionFailed) {
      sessionFailed=true;
      if(cycleRunning || pendingUpload)attemptFailed(modem.status);
      else recoveryFailed(netRecovery,modem.status);
    }
    rawQueued=false;
  }
  if(!modem.busy() && modem.shutdownAttempted && !shutdownEpisodeHandled) {
    shutdownEpisodeHandled=true;shutdownFault=!modem.shutdownConfirmed;
    if(shutdownFault) {netRecovery.defer(cfg.retryMaxSeconds,millis(),epochNow());log("模组关机未确认，保存电源未知故障并短退避");}
    if(!closingNetwork && !remoteOnline && !sessionFailed) {
      sessionFailed=true;
      if(cycleRunning || pendingUpload)attemptFailed(modem.status);
      else recoveryFailed(netRecovery,modem.status);
    }
    closingNetwork=false;saveRuntime();
  }
  now=epochNow();
  if(restartPending || closingNetwork)return;
  if(cfg.sleep && now) {
    auto window=bt::workWindow(cfg,now,remote.completed);
    if(!remote.slot && window.active()) {
      remote.slot=window.start;remote.slotEnd=window.end;remote.attempts=0;remote.retryAt=0;remote.slotReason.clear();
      if(!saveRemote())return;
    }
    if(remote.slot && now<remote.slotEnd && !cycleRunning && !pendingUpload && !rawQueued && (!modem.busy() || modem.ready()) && now>=remote.retryAt && remote.attempts<cfg.retryAttempts && netRecovery.allowed(millis())) {
      ++remote.attempts;
      if(saveRemote() && !startScan(true))attemptFailed("扫描无法启动或目标未配置");
    }
  } else if(!cfg.sleep && !cycleRunning && !pendingUpload && !rawQueued && (!modem.busy() || modem.ready()) && !cfg.targetMac.empty() && due(nextScan) && netRecovery.allowed(millis())) {
    if(!startScan(due(nextUpload)))attemptFailed("扫描无法启动");
  }
  bool needNetwork=!cfg.sleep || !now || cycleRunning || pendingUpload || (bt::syncDue(now,lastSyncEpoch) && timeRecovery.allowed(millis()));
  if(needNetwork && !modem.busy() && netRecovery.allowed(millis())) {
    slotAborted=false;beginModem();
  }
  // Cold boot or time loss gets one bounded synchronization/control session.
  // A known-clock scheduled idle period does not manufacture an extra upload.
}

void slaveTick() {
  if(!storageOk) {subscriber.stop();advertiser.stop();return;}
  if(WiFi.status()==WL_CONNECTED && (!cfg.tls || epochNow()) && netRecovery.allowed(millis())) {
    if(!subscriber.active()) {
      mqttStarted=millis();mqttHealthyAt=0;
      if(!subscriber.start(cfg,true))recoveryFailed(netRecovery,"MQTT 参数/客户端初始化失败");
    } else if(subscriber.failed || (!subscriber.subscribed && since(mqttStarted)>=cfg.connectSeconds*1000UL)) {
      subscriber.stop();mqttHealthyAt=0;recoveryFailed(netRecovery,"MQTT 连接/订阅失败");
    } else if(subscriber.subscribed) {
      if(!mqttHealthyAt)mqttHealthyAt=millis();
      if(since(mqttHealthyAt)>=60000 && netRecovery.success())saveRuntime();
    }
  }
  std::string raw,incomingTopic;
  if(subscriber.popPacket(incomingTopic,raw)) {
    if(incomingTopic!=bt::topic(cfg,"raw")) {receiveCommand(incomingTopic,raw);return;}
    if(cfg.fixed)return;
    bt::Message incoming;std::string error;
    if(!bt::decodeMessage(raw,incoming,error) || !bt::frameValid(incoming.frame,cfg.strict,error)) {log("MQTT 消息被拒绝："+error);return;}
    auto decision=bt::freshness(incoming,hasSaved?&saved:nullptr,cfg.masterId,cfg.targetMac,cfg.generation);
    if(decision==bt::Freshness::Reject || (epochNow() && incoming.observed>epochNow()+300)) {log("MQTT 主编号/MAC/代次/序号/时间校验未通过");return;}
    lastReceivedEpoch=epochNow();receivedMillis=millis();
    if(decision==bt::Freshness::New) {
      if(!saveSnapshot(incoming)) {log("MQTT 缓存写入失败，保留旧广播");return;}
      saved=incoming;hasSaved=true;startAdvertising();log("已接收主端快照，采集时间："+stamp(saved.observed));
      remote.record(epochNow(),advertiser.active(),advertiser.active()?"收到新快照并更新广播":"收到新快照，但广播未启动");saveRemote();queueRemote();
    } else log("收到重复快照，保留原采集时间");
  }
  if(simulationEnabled && !advertiser.active() && since(lastAdvRetry)>=10000) {lastAdvRetry=millis();startAdvertising();}
}
#ifndef BT_RUNTIME_TEST
void statusJson(JsonDocument& d) {
  d["firmwareVersion"]="0.3.7-dev+r1";d["configRevision"]=cfg.revision;d["powerSaving"]=IsMaster && cfg.sleep;d["remoteConnected"]=remoteOnline;
  d["maintenanceWindowActive"]=maintenanceActive();d["maintenanceWindowSeconds"]=maintenanceActive()?maintenanceWindow.remaining(millis()):0;
  d["configRecovery"]=configRecovery;d["configStatus"]=configStatus;d["simStatus"]=modem.simStatus;
  d["storageBackend"]=store.backend();d["storageError"]=store.lastError();d["storageMigrated"]=store.migrated();
  d["screenDetected"]=screenOk;d["modemFailureStep"]=modem.failureStep;d["modemErrorCode"]=modem.errorCode;d["modemNetworkClock"]=modem.networkClock;
  d["modemConnectionCode"]=modem.connectionCode;
  d["modemRxBytes"]=modem.receivedBytes;d["modemPowerKeyHigh"]=IsMaster && digitalRead(5)==HIGH;
  d["modemReadShape"]=modem.lastReadShape;d["modemReadFailureShape"]=modem.lastReadFailureShape;d["modemEmptyReadShape"]=modem.lastEmptyReadShape;d["modemReadFailures"]=modem.readFailures;
  auto window=bt::workWindow(cfg,epochNow(),remote.completed);d["nextWindow"]=window.active()?window.start:window.next;d["activeWindowEnd"]=remote.slotEnd;
  d["displayLockEnabled"]=cfg.displayLock;d["displayLocked"]=displayAccess.locked();
  d["lastTimeSync"]=stamp(lastSyncEpoch);d["timeSyncIntervalSeconds"]=bt::DaySeconds;
  d["nextTimeSyncSeconds"]=bt::syncDelay(epochNow(),lastSyncEpoch);
  d["timeSyncActive"]=ntpRunning || timeJob;
  d["lastAutomaticRestart"]=stamp(lastAutoRestart);d["bootFaults"]=bootFaults;
  auto recovery=[&](const char* key,const bt::Recovery& r) {
    auto v=d[key].to<JsonObject>();v["attemptsInBatch"]=r.attempts;v["failedBatches"]=r.batches;
    v["waitSeconds"]=r.remaining(millis());v["cooling"]=false;
  };
  recovery("networkRecovery",netRecovery);recovery("wifiRecovery",wifiRecovery);recovery("timeRecovery",timeRecovery);
  d["role"]=IsMaster?"master":"slave";d["deviceId"]=cfg.deviceId;d["masterId"]=cfg.masterId;
  d["wifi"]=WiFi.status()==WL_CONNECTED;d["wifiIp"]=WiFi.localIP().toString();d["wifiTest"]=wifiTest;
  d["wifiDisconnectReason"]=wifiDisconnectReason;
  d["mqttConfigIssue"]=cfg.broker.empty()?"尚未设置服务器域名":cfg.mqttUser.empty()?"尚未设置MQTT用户名":cfg.tls && cfg.caPem.empty()?"TLS已开启，但根CA证书为空，暂不能连接MQTT":"";
  d["ap"]=apOn;d["apIp"]="192.168.4.1";d["cellular"]=IsMaster && modem.registered;
  d["mqtt"]=IsMaster?modem.connected:subscriber.connected;d["subscribed"]=!IsMaster && subscriber.subscribed;
  d["modemStatus"]=modem.status;d["modemIdentity"]=modem.identity;d["modemShutdownConfirmed"]=modem.shutdownConfirmed;
  d["modemShutdownFault"]=shutdownFault;d["modemShutdownAttempts"]=modem.shutdownAttempts;d["modemShutdownError"]=modem.shutdownError;
  d["pendingUpload"]=pendingUpload;
  auto binding=d["binding"].to<JsonObject>();binding["masterId"]=cfg.masterId;binding["prefix"]=cfg.prefix;binding["targetMac"]=cfg.targetMac;binding["generation"]=cfg.generation;
  d["scan"]=scanState;d["scanActive"]=scanner.active();d["targetSeenReports"]=scanner.seen();
  d["upload"]=uploadState;d["lastUpload"]=stamp(lastUploadEpoch);d["lastObserved"]=stamp(hasSaved?saved.observed:0);
  d["receivedAt"]=stamp(lastReceivedEpoch);d["timeValid"]=epochNow()!=0;d["bleAdvertising"]=advertiser.active();d["bleError"]=bleError;
  d["mode"]=IsMaster?"采集上传":cfg.fixed?"固定模式":"从设备模式";d["uptimeSeconds"]=millis()/1000;d["freeHeap"]=ESP.getFreeHeap();
  d["nextScanSeconds"]=due(nextScan)?0:(nextScan-millis())/1000;d["nextUploadSeconds"]=due(nextUpload)?0:(nextUpload-millis())/1000;
  d["storageOk"]=storageOk;d["freeNvsEntries"]=store.freeEntries();d["batteryEstimated"]=true;int pct=battery.percent(cfg.usableMah);
  if(IsMaster && cfg.batteryMode && pct>=0)d["batteryEstimatePercent"]=pct;else d["batteryEstimatePercent"]=nullptr;
  d["power"]=IsMaster && cfg.batteryMode?"电池供电（电量粗估）":"外接供电";
  bool matches=false;const bt::Frame* f=cfg.fixed && !IsMaster?&cfg.fixedFrame:hasSaved?&saved.frame:nullptr;
  d["rolling"]=f?bt::rolling(*f,cfg.rollOffset,cfg.rollLength,cfg.rollMirror,matches):"";d["rollingCopiesMatch"]=matches;
  std::string cache=!hasSaved?"尚无缓存":!saved.observed?"缓存采集时间未知":!epochNow()?"当前时间未知，缓存年龄未知":epochNow()>saved.observed && epochNow()-saved.observed>cfg.staleSeconds?"缓存已过期，仍继续重播":"已缓存";
  if(!IsMaster && cfg.fixed)cache="固定 raw，不随 MQTT 更新";
  else if(!IsMaster && hasSaved && !subscriber.connected)cache+=" / 离线重播";
  d["cacheState"]=cache;
  if(hasSaved) {JsonDocument packet;bt::messageJson(saved,packet,-1);d["snapshot"]=packet.as<JsonObject>();d["rawHex"]=bt::hex(saved.frame.adv)+bt::hex(saved.frame.rsp);}
  else d["snapshot"]=nullptr;
  JsonDocument nearby;deserializeJson(nearby,scanner.environment());d["nearby"]=nearby.as<JsonArray>();
  auto a=d["logs"].to<JsonArray>();for(auto& s:logs)a.add(s);
}
void initWeb() {
  const char* headers[]={"X-CSRF-Token","Accept-Encoding"};web.collectHeaders(headers,2);
  web.on("/",HTTP_GET,[]{
    if(!auth())return;
    bool compressed=web.header("Accept-Encoding").indexOf("gzip")>=0;
    const char* bytes=compressed?reinterpret_cast<const char*>(WEB_PAGE_GZIP):WEB_PAGE;
    size_t length=compressed?sizeof(WEB_PAGE_GZIP):sizeof(WEB_PAGE)-1;
    web.sendHeader("Cache-Control","no-store");web.sendHeader("Vary","Accept-Encoding");
    if(compressed)web.sendHeader("Content-Encoding","gzip");
    web.setContentLength(length);web.send(200,"text/html; charset=utf-8","");
    auto client=web.client();uint32_t started=millis();
    bool sent=bt::streamWebBytes(bytes,length,[&](const char* data,size_t count) {
      if(!client.connected() || since(started)>20000)return false;
      return client.write(reinterpret_cast<const uint8_t*>(data),count)==count;
    });
    if(!sent)client.stop();
  });
  web.on("/api/status",HTTP_GET,[]{if(!auth())return;JsonDocument d;statusJson(d);sendJson(200,d);});
  web.on("/api/config",HTTP_GET,[]{if(!auth())return;JsonDocument d;bt::configJson(cfg,d,false);d["csrfToken"]=csrf;sendJson(200,d);});
  web.on("/api/backup",HTTP_GET,[]{
    if(!auth(true))return;
    if(configRecovery) {reply(409,"当前配置不可用，请先恢复备份");return;}
    JsonDocument d;configBackup(d);sendJson(200,d);
  });
  web.on("/api/restore",HTTP_POST,[]{
    if(!auth(true))return;JsonDocument d;if(!body(d))return;
    if(!d["expectedRevision"].is<uint32_t>()) {reply(400,"缺少当前配置版本");return;}
    JsonDocument result;int code=restoreLocalConfig(d,d["expectedRevision"].as<uint32_t>(),result);sendJson(code,result);
  });
  web.on("/api/config",HTTP_POST,[]{
    if(!auth(true))return;JsonDocument d;if(!body(d))return;
    JsonDocument result;int code=saveLocalConfig(d,result);sendJson(code,result);
  });
  web.on("/api/action",HTTP_POST,[]{
    if(!auth(true))return;JsonDocument d;if(!body(d))return;
    if(!d["action"].is<std::string>()) {reply(400,"缺少操作名称");return;}auto action=d["action"].as<std::string>();
    screenUntil=millis()+cfg.screenSeconds*1000UL;
    if(action=="restart") {reply(200,"即将重启");restartForMaintenance=true;restartPending=true;restartAt=millis()+1000;}
    else if(action=="reset") {reply(200,"即将正常结束联网并清除配置、缓存，维护密码将恢复通用初始值");restartForMaintenance=true;factoryReset=restartPending=true;restartAt=millis()+1000;}
    else if(action=="maintenance-end" && IsMaster) {reply(200,"已结束维护，热点即将关闭，随后按原工作计划运行");requestMaintenanceExit();}
    else if(action=="battery-full" && IsMaster) {battery.usedMah=0;battery.calibrated=true;if(saveRuntime())reply(200,"已重置为估算100%");else reply(500,"估算写入失败");}
    else if(action=="toggle" && !IsMaster) {simulationEnabled=!simulationEnabled;startAdvertising();reply(200,simulationEnabled?"模拟已开启":"模拟已停止（重启恢复配置值）");}
    else if(action=="wifi-test") {if(!wifiRecovery.allowed(millis()) || wifiConnecting) {reply(429,"WiFi 正在尝试或处于短退避，请等待");return;}if(cfg.wifiSsid.empty()) {reply(400,"请先保存 WiFi 参数");return;}subscriber.stop();wasWifiConnected=false;WiFi.disconnect(false);WiFi.begin(cfg.wifiSsid.c_str(),cfg.wifiPass.c_str());wifiAttempt=millis();wifiConnecting=true;wifiTest="测试中";reply(200,"WiFi 测试已开始，状态页显示结果");}
    else if(action=="network-test" && IsMaster) {if(scanner.active() || cycleRunning || modem.busy() || !netRecovery.allowed(millis())) {reply(409,"采集、联网正在运行或尚在短退避");return;}networkTest=true;timeJob=false;cycleRunning=beginModem(true);reply(200,"4G 检测已开始");}
    else if((action=="scan" || action=="upload") && IsMaster) {if(!startScan(action=="upload"))reply(409,"设备忙、蓝牙不可用或目标 MAC 未配置");else reply(200,"扫描已开始");}
    else reply(400,"当前角色不支持此操作");
  });
  web.onNotFound([]{reply(404,"路径不存在");});if(apOn)web.begin();
}
void drawScreen() {
  if(due(screenUntil)) {
    if(!displayOff) {displayOff=true;lockScreen();if(screenOk)screen.setPowerSave(1);}
    return;
  }
  if(!screenOk || since(lastScreen)<250)return;
  lastScreen=millis();
  ScreenView v;v.master=IsMaster;v.wifi=WiFi.status()==WL_CONNECTED;v.cell=modem.registered;
  v.mqtt=IsMaster?modem.connected:subscriber.subscribed;v.ble=IsMaster?scanner.active():advertiser.active();
  v.locked=displayAccess.locked();v.page=displayAccess.page;v.epoch=epochNow();v.id=cfg.deviceId;
  if(IsMaster && cfg.batteryMode)v.battery=battery.percent(cfg.usableMah);
  bool matches=false;const bt::Frame* f=!IsMaster && cfg.fixed?&cfg.fixedFrame:hasSaved?&saved.frame:nullptr;
  if(f)v.rolling=bt::rolling(*f,cfg.rollOffset,cfg.rollLength,cfg.rollMirror,matches);
  const char* masterTitles[]={"时钟","总览","当前滚动码","蓝牙采集","MQTT上传","数据记录","工作计划","设备维护"};
  const char* slaveTitles[]={"时钟","总览","当前滚动码","蓝牙发射","MQTT接收","数据来源","网络详情","设备维护"};
  v.title=IsMaster?masterTitles[v.page]:slaveTitles[v.page];
  if(v.page==2)v.lines[2]=!v.rolling.empty()?(matches?"RAW / MIRROR OK":"RAW / CHECK COPY"):"NO VALID DATA";
  else if(v.page==3) {
    if(IsMaster && hasSaved)v.title+=" "+std::to_string(saved.rssi);
    v.lines[0]=IsMaster?scanState:advertiser.active()?"正在广播":"未发射";
    v.lines[1]=cfg.targetMac;v.lines[2]=IsMaster?"RSSI "+std::to_string(saved.rssi):cfg.fixed?"固定模式":"跟随主设备";
  } else if(v.page==4) {
    v.lines[0]=IsMaster?uploadState:subscriber.subscribed?"订阅成功":"订阅未就绪";
    v.lines[1]=stamp(IsMaster?lastUploadEpoch:lastReceivedEpoch);v.lines[2]="采集 "+stamp(hasSaved?saved.observed:0);
  } else if(v.page==5) {
    v.lines[0]=!IsMaster && cfg.fixed?"固定模式":"主 "+cfg.masterId;v.lines[1]=stamp(hasSaved?saved.observed:0);
    v.lines[2]=hasSaved?stamp(saved.observed):"尚无有效数据";
  } else if(v.page==6) {
    if(IsMaster) {
      if(cfg.sleep) {
        auto w=bt::workWindow(cfg,epochNow(),remote.completed);
        v.lines[0]=remote.slot?"本段至 "+stamp(remote.slotEnd):"下次 "+stamp(w.next);
        v.lines[1]=remote.slot?"尝试 "+std::to_string(remote.attempts)+"/"+std::to_string(cfg.retryAttempts):"每日 "+std::to_string(cfg.slots.size())+" 个时段";
      } else {
        v.lines[0]="扫描 "+std::to_string(bt::until(millis(),nextScan)/1000)+"秒";
        v.lines[1]="上传 "+std::to_string(bt::until(millis(),nextUpload)/1000)+"秒";
      }
    } else {v.lines[0]=WiFi.localIP().toString().c_str();v.lines[1]=subscriber.subscribed?"MQTT 在线":"MQTT 等待恢复";}
    v.lines[2]="校时 "+stamp(lastSyncEpoch);
  } else if(v.page==7) {
    v.lines[0]=apOn?"后台 192.168.4.1":"长按3秒打开后台";
    v.lines[1]=shutdownFault?"模组关机未确认":"恢复等待 "+std::to_string(netRecovery.remaining(millis()))+"秒";
    v.lines[2]=cfg.displayLock?"息屏后重新锁定":"显示锁已关闭";
  }
  renderScreen(screen,v);
  screen.setPowerSave(0); // Replace the old frame BEFORE lighting the panel.
}
void buttonTick() {
  bool low=!digitalRead(BUTTON_PIN);
  if(low) {
    if(!buttonAt && since(buttonHighAt)>50) {buttonAt=millis();buttonLatched=false;}
    if(buttonAt && since(buttonAt)>=3000 && !buttonLatched) {
      buttonLatched=true;
      if(!displayAccess.locked()) {screenUntil=millis()+cfg.screenSeconds*1000UL;displayOff=false;displayAccess.page=7;startAp();}
      else displayAccess.release(3000,millis());
    }
  } else {
    if(buttonAt && !buttonLatched) {
      uint32_t duration=since(buttonAt);
      if(duration>=50) {
          maintenanceActivity();
        bool waking=displayOff || due(screenUntil);
        if(waking) {lockScreen();displayOff=false;screenUntil=millis()+cfg.screenSeconds*1000UL;}
        if(displayAccess.locked()) {
          displayAccess.release(duration,millis());
          screenUntil=millis()+std::max<uint32_t>(cfg.screenSeconds,12)*1000UL;
        }
        else if(!waking && duration<=600)displayAccess.page=(displayAccess.page+1)%8;
        // Silent while locked: no progress, error, or hotspot feedback.
        if(!displayAccess.locked())screenUntil=millis()+cfg.screenSeconds*1000UL;
        lastScreen=millis()-250;
      }
    }
    buttonAt=0;buttonLatched=false;buttonHighAt=millis();
  }
}
void networkTick() {
  bool online=WiFi.status()==WL_CONNECTED;
  if(online) {
    wifiConnecting=false;wifiTest="连接成功";
    if(!wifiHealthyAt)wifiHealthyAt=millis();
    if(since(wifiHealthyAt)>=60000 && wifiRecovery.success())saveRuntime();
  } else {
    wifiHealthyAt=0;
    if(wasWifiConnected || (wifiConnecting && since(wifiAttempt)>=cfg.connectSeconds*1000UL)) {
      wifiConnecting=false;subscriber.stop();mqttHealthyAt=0;
      WiFi.disconnect(false);wifiTest="连接失败，已进入有限恢复";
      recoveryFailed(wifiRecovery,"WiFi 连接失败/中断");
    }
    if(!cfg.wifiSsid.empty() && !wifiConnecting && wifiRecovery.allowed(millis()) && !restartPending) {
      WiFi.begin(cfg.wifiSsid.c_str(),cfg.wifiPass.c_str());wifiAttempt=millis();wifiConnecting=true;wifiTest="连接中";
    }
  }
  wasWifiConnected=online;
  // Start SNTP only for a due daily job; stop it after success or bounded timeout.
  // The network callback sets a flag, never NVS or UI state.
  if(ntpReceived) {
    ntpReceived=false;
    if(ntpRunning && time(nullptr)>=1704067200 && time(nullptr)<=4102444800LL) {
      clockValid=true;lastSyncEpoch=epochNow();timeRecovery.success();saveRuntime();
      log("WiFi 网络校时成功");sntp_stop();ntpRunning=false;
    }
  }
  if(ntpRunning && (!online || since(ntpStarted)>=cfg.connectSeconds*1000UL)) {
    sntp_stop();ntpRunning=false;recoveryFailed(timeRecovery,"WiFi 校时失败");
  }
  if(online && !ntpRunning && bt::syncDue(epochNow(),lastSyncEpoch) && timeRecovery.allowed(millis()) && !modem.busy()) {
    ntpReceived=false;ntpStarted=millis();ntpRunning=true;
    configTime(8*3600,0,"ntp.aliyun.com","ntp.tencent.com");
  }
}

void batteryTick() {
  uint32_t dt=since(lastBattery);lastBattery=millis();
  if(IsMaster && cfg.batteryMode)battery.charge(cfg.activeMa+(modem.busy()?cfg.modemMa:0),dt/1000.0);
  if(since(lastPersist)>=600000) {saveRuntime();if(dirtyCache && hasSaved && saved.seq) {saveSnapshot(saved);dirtyCache=false;}}
}
#endif
#include "remote_runtime.inc"
void maybeSleep() {
  if(!IsMaster || !cfg.sleep || maintenanceActive() || maintenanceExitPending || (buttonAt && since(buttonAt)<4000) || apOn || restartPending || cycleRunning || scanner.active() || modem.busy() || pendingUpload || !storageOk)return;
  if(!remoteOnline) {outgoing.clear();txActive=false;}
  if(!outgoing.empty())return;
  uint64_t now=epochNow(),next=scheduledWake();
  uint32_t wait=now && next>now?uint32_t(std::min<uint64_t>(next-now,86400)):std::max<uint32_t>(60,netRecovery.remaining(millis()));
  if(!now)wait=std::max<uint32_t>(wait,timeRecovery.remaining(millis()));
  if(wait<1 || ntpRunning || wifiConnecting)return;
  sleepPending=true;if(!saveRuntime() || !saveRemote()) {sleepPending=false;return;}
  slept.magic=0x42544331;slept.elapsedUs=static_cast<uint64_t>(wait)*1000000;slept.used=battery.usedMah;
  slept.crc=bt::checksum(std::to_string(slept.elapsedUs)+":"+std::to_string(slept.used));
  lockScreen();scanner.stop();screen.setPowerSave(1);WiFi.disconnect(true);WiFi.mode(WIFI_OFF);modem.prepareSleep();
  if(!modem.shutdownConfirmed)log("进入定时睡眠；模组关机尚无物理状态确认");
  gpio_hold_dis(GPIO_NUM_5);digitalWrite(5,LOW);gpio_hold_en(GPIO_NUM_5);gpio_deep_sleep_hold_en();
  esp_sleep_enable_timer_wakeup(slept.elapsedUs);Serial.flush();esp_deep_sleep_start();
}
}

#ifndef BT_RUNTIME_TEST
void setup() {
  // Blank the retained OLED framebuffer early, before loading private data or BLE.
  Wire.begin(SCREEN_SDA,SCREEN_SCL);Wire.setTimeOut(50);
  for(uint8_t address:{0x3C,0x3D}) {
    Wire.beginTransmission(address);
    if(Wire.endTransmission()==0) {
      screen.setI2CAddress(address*2);screen.begin();screen.enableUTF8Print();
      screen.clearBuffer();screen.sendBuffer();screen.setPowerSave(1);screenOk=true;break;
    }
  }
  Serial.begin(115200);delay(200);setenv("TZ","CST-8",1);tzset();
  pinMode(BUTTON_PIN,INPUT_PULLUP);buttonHighAt=millis();
  bool timerWake=esp_sleep_get_wakeup_cause()==ESP_SLEEP_WAKEUP_TIMER;
  auto resetReason=esp_reset_reason();
  bool requestedMaintenance=nextBootMaintenance==MaintenanceBootMagic;nextBootMaintenance=0;
  bt::BootKind bootKind=timerWake?bt::BootKind::Timer:resetReason==ESP_RST_POWERON?bt::BootKind::PowerOn:resetReason==ESP_RST_EXT?bt::BootKind::ResetButton:resetReason==ESP_RST_SW && requestedMaintenance?bt::BootKind::RequestedRestart:resetReason==ESP_RST_SW?bt::BootKind::AutomaticRestart:bt::BootKind::Fault;
  clockValid=timerWake && time(nullptr)>=1704067200;
  if(IsMaster) {gpio_deep_sleep_hold_dis();gpio_hold_dis(GPIO_NUM_5);modem.begin();}
  storageOk=store.begin();csrf=randomText(32);
  uint64_t mac=ESP.getEfuseMac();char id[20];snprintf(id,sizeof(id),"%s-%06lx",IsMaster?"master":"slave",static_cast<unsigned long>(mac&0xffffff));
  cfg.sleep=IsMaster;cfg.deviceId=id;cfg.prefix="pair-"+randomText(12);cfg.apPass=cfg.adminPass=DEFAULT_MAINTENANCE_PASSWORD;
  std::string text,error;bool configured=loadConfiguration();
  if(!configured) {
    Serial.println(configStatus.c_str());
  }
  Serial.printf("CONFIG loaded=%u revision=%lu wifi_set=%u broker_set=%u mqtt_user_set=%u ca_bytes=%u\n",configured,static_cast<unsigned long>(cfg.revision),!cfg.wifiSsid.empty(),!cfg.broker.empty(),!cfg.mqttUser.empty(),unsigned(cfg.caPem.size()));
  if(storageOk && store.read("runtime",text)) {
    JsonDocument d;if(!deserializeJson(d,text)) {
      double used=d["used"]|0.0;if(std::isfinite(used) && used>=0) {battery.usedMah=used;battery.calibrated=d["calibrated"]|false;}
      sleepPending=d["sleepPending"]|false;
      pendingUpload=d["pendingUpload"]|false;shutdownFault=d["shutdownFault"]|false;
      lastSyncEpoch=d["lastSync"]|0ULL;lastAutoRestart=d["lastAutoRestart"]|0ULL;
      bootFaults=d["bootFaults"]|0UL;
      auto restore=[&](const char* key,bt::Recovery& r) {
        JsonObjectConst v=d[key].as<JsonObjectConst>();r.restore(cfg,millis(),epochNow(),v["attempts"]|0UL,v["batches"]|0UL,v["retryEpoch"]|0ULL,v["waiting"]|false);
      };
      bool sameWifi=d["wifiGeneration"].as<uint32_t>()==cfg.wifiGeneration;
      bool sameMqtt=d["mqttGeneration"].as<uint32_t>()==cfg.mqttGeneration;
      if(sameMqtt || shutdownFault || bootFaults>=3)restore("netRecovery",netRecovery);
      if(sameWifi)restore("wifiRecovery",wifiRecovery);
      if(sameWifi && sameMqtt)restore("timeRecovery",timeRecovery);
      if(!sameWifi || !sameMqtt)log("网络参数代次变化，旧网络的失败等待不带入新参数；尝试仍有次数上限");
      if(d["master"].as<std::string>()==cfg.masterId && d["mac"].as<std::string>()==cfg.targetMac && d["generation"].as<uint32_t>()==cfg.generation) {lastScanEpoch=d["lastScan"]|0ULL;lastUploadEpoch=d["lastUpload"]|0ULL;failures=d["failures"]|0UL;}
    }
  }
  bool validSleep=timerWake && slept.magic==0x42544331 && slept.crc==bt::checksum(std::to_string(slept.elapsedUs)+":"+std::to_string(slept.used));
  if(validSleep) {
    if(IsMaster && cfg.batteryMode) {battery.usedMah=std::max(battery.usedMah,slept.used);battery.charge(cfg.sleepMa,slept.elapsedUs/1000000.0);}
  }
  if(sleepPending && !validSleep && cfg.batteryMode) {battery.calibrated=false;log("睡眠被复位/断电打断，累计耗电保留，电量标为未校准");}
  if(sleepPending) {sleepPending=false;saveRuntime();}
  slept.magic=0;
  if(storageOk && store.read("snapshot",text)) {
    JsonDocument d;
    if(!deserializeJson(d,text) && d["prefix"].as<std::string>()==cfg.prefix && d["broker"].as<std::string>()==cfg.broker && d["port"].as<int>()==cfg.port && bt::decodeMessage(d["message"].as<std::string>(),saved,error)) {
      hasSaved=saved.master==cfg.masterId && saved.frame.mac==cfg.targetMac && saved.generation==cfg.generation && bt::frameValid(saved.frame,cfg.strict,error);
    }
  }
  if(storageOk && store.read("remote",text) && !remote.decode(text)) {storageOk=false;log("远程记录损坏，禁止远程执行及自动任务，需本地维护");}
  if(remote.slot && epochNow() && epochNow()>=remote.slotEnd) {remote.finishSlot(epochNow(),false,"复位期间时段结束，补报失败");saveRemote();}
  for(auto& receipt:remote.receipts)if(receipt.state=="running") {
    receipt.state=receipt.command.action=="restart" || cfg.appliedCommand==receipt.command.id?"done":"failed";
    receipt.reason=receipt.state=="done"?"重启后确认":"执行被复位打断，未自动重放";receipt.at=epochNow();
  }
  saveRemote();
  displayAccess.configure(cfg.displayLock);
  simulationEnabled=cfg.simulate;
  bleOk=NimBLEDevice::init(IsMaster?"BT Capture":"BT Replay") && scanner.begin();
  bool fault=resetReason==ESP_RST_PANIC || resetReason==ESP_RST_INT_WDT || resetReason==ESP_RST_TASK_WDT || resetReason==ESP_RST_WDT || resetReason==ESP_RST_BROWNOUT;
  if(fault) {
    bootFaults=std::min<uint32_t>(bootFaults+1,1000000);
    if(bootFaults>=3) {
      for(auto* r:{&netRecovery,&wifiRecovery,&timeRecovery})r->restore(cfg,millis(),0,r->attempts,r->batches,0,true);
      log("连续异常复位，联网进入短退避");
    }
    saveRuntime();
  }
  WiFi.persistent(false);WiFi.mode(WIFI_STA);WiFi.setAutoReconnect(false);
  WiFi.onEvent([](arduino_event_id_t event,arduino_event_info_t info){
    if(event==ARDUINO_EVENT_WIFI_STA_DISCONNECTED)wifiDisconnectReason=info.wifi_sta_disconnected.reason;
    else if(event==ARDUINO_EVENT_WIFI_STA_GOT_IP)wifiDisconnectReason=0;
  });

  if(!configured || cfg.targetMac.empty() || (!IsMaster && cfg.wifiSsid.empty()) || cfg.broker.empty())startAp();
  sntp_set_time_sync_notification_cb([](struct timeval*){ntpReceived=true;});
  sntp_set_sync_interval(86400000UL);
  initWeb();lastBattery=lastPersist=millis();screenUntil=millis()+cfg.screenSeconds*1000UL;
  beginMaintenanceWindow(bootKind);
  nextScan=nextUpload=millis();
  if(timerWake && epochNow() && !failures) {
    uint64_t now=epochNow();if(lastScanEpoch && now>=lastScanEpoch && now-lastScanEpoch<cfg.scanEvery)nextScan=millis()+static_cast<uint32_t>(cfg.scanEvery-(now-lastScanEpoch))*1000UL;
    if(lastUploadEpoch && now>=lastUploadEpoch && now-lastUploadEpoch<cfg.uploadEvery)nextUpload=millis()+static_cast<uint32_t>(cfg.uploadEvery-(now-lastUploadEpoch))*1000UL;
  }
  if(IsMaster && pendingUpload)nextUpload=millis();
  if(IsMaster && shutdownFault && !netRecovery.waiting)netRecovery.defer(cfg.retryMaxSeconds,millis(),epochNow());
  if(netRecovery.waiting && IsMaster) {
    if(failures)nextScan=nextUpload=netRecovery.deadline;
    else if(pendingUpload)nextUpload=netRecovery.deadline;
  }
  if(!IsMaster)startAdvertising();
  esp_task_wdt_init(30,true);esp_task_wdt_add(nullptr);
  log(IsMaster?"主端启动：只采集和上传":"从端启动：固定/跟随模式");
  log("软件开发验证版；整机验收尚未完成");
}
void loop() {
  // Expire the display permission before handling a pending HTTP request or key.
  if(!displayOff && due(screenUntil)) {displayOff=true;lockScreen();if(screenOk)screen.setPowerSave(1);}
  buttonTick();
  usbTick();
  web.handleClient();
  maintenanceTick();
  if(apOn) {
    if(WiFi.softAPgetStationNum()) {apIdle=millis();screenUntil=millis()+cfg.screenSeconds*1000UL;}
    else if(since(apIdle)>cfg.apSeconds*1000UL) {closeAp();log("维护热点闲置超时，已关闭");}
  }
  networkTick();
  if(bootFaults && millis()>=600000) {bootFaults=0;saveRuntime();}
  if(autoRestartPending && !restartPending && !cycleRunning && !modem.busy()) {
    autoRestartPending=false;
    uint32_t batches=std::max(netRecovery.batches,std::max(wifiRecovery.batches,timeRecovery.batches));
    if(bt::mayRestart(cfg,batches,epochNow(),lastAutoRestart)) {
      lastAutoRestart=epochNow();
      if(saveRuntime()) {restartForMaintenance=false;restartPending=true;restartAt=millis()+1000;log("有限自动重启，退避期限保留；24小时内不再自动重启");}
    }
  }
  batteryTick();
  if(IsMaster)masterTick();else slaveTick();
  remoteTick();
  if(scanClockPending && epochNow()) {
    lastScanEpoch=epochNow()-since(scanEnded)/1000;scanClockPending=false;saveRuntime();
  }
  drawScreen();
  if(restartPending) {
    if(due(restartAt) && IsMaster && modem.busy())modem.shutdown();
    if(due(restartAt) && (!IsMaster || !modem.busy())) {
      if(factoryReset) {if(!store.clear()) {restartPending=false;storageOk=false;configRecovery=true;configStatus="恢复出厂未完成，业务已停止，请重启继续清理";log(configStatus);return;}}
      else saveRuntime();
      nextBootMaintenance=restartForMaintenance?MaintenanceBootMagic:0;
      ESP.restart();
    }
  }
  esp_task_wdt_reset();maybeSleep();delay(5);
}

#endif
