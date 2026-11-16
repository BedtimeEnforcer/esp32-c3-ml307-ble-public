#include "protocol.h"
#include "remote.h"
#include <algorithm>
#include <cmath>

namespace bt {
static std::string upper(std::string s) { for (char& c:s) c=static_cast<char>(toupper(static_cast<unsigned char>(c))); return s; }
static bool hostValid(const std::string& s) {
  if (s.empty()) return true;
  if (s.size()>128 || s.front()=='.' || s.back()=='.') return false;
  for (unsigned char c:s) if (!isalnum(c) && c!='.' && c!='-') return false;
  return true;
}
bool configValid(const Config& c, bool master, std::string& e) {
  e.clear();
  if(!slotsValid(c.slots,e))return false;
  if(!master && c.sleep) {e="从端固定常在线，不支持深睡";return false;}
  if(!c.revision || !c.wifiGeneration || !c.mqttGeneration || c.appliedCommand.size()>40) {e="配置版本非法";return false;}
  if (c.schema!=1 || !c.generation) e="配置版本或数据代次不合法";
  else if (!safeToken(c.deviceId,32) || !safeToken(c.masterId,32) || !safeToken(c.prefix,32)) e="设备编号/主编号/主题前缀仅允许字母数字、横线、下划线，1～32位";
  else if (!c.targetMac.empty() && !staticMac(c.targetMac)) e="目标 MAC 必须为可重播的随机静态地址";
  else if (!hostValid(c.broker) || c.port<1 || c.port>65535) e="MQTT 域名或端口非法";
  else if (!c.tls && !c.labPlaintext) e="明文 MQTT 仅可显式启用实验室测试模式";
  else if (!atSafe(c.mqttUser,128) || !atSafe(c.mqttPass,256) || !atSafe(c.apn,100) || !safeToken(c.modemCa,32)) e="AT 参数包含不支持字符或超长";
  else if (c.wifiSsid.size()>32 || c.wifiPass.size()>63 || (!c.wifiPass.empty() && c.wifiPass.size()<8)) e="WiFi SSID 最多32字节，密码8～63字节（开放网络可空）";
  else if (c.apPass.size()<8 || c.apPass.size()>63 || c.adminPass.size()<8 || c.adminPass.size()>63) e="维护热点与后台密码需8～63字节";
  else if (c.caPem.size()>4096 || (!c.caPem.empty() && c.caPem.find("-----BEGIN CERTIFICATE-----")==std::string::npos)) e="CA 应为 PEM，最多4096字节";
  else if (c.rollLength<0 || c.rollLength>8 || c.rollOffset<0 || c.rollOffset+c.rollLength>62 || c.rollMirror < -1 || (c.rollMirror>=0 && c.rollMirror+c.rollLength>62)) e="滚动码区间超出 raw，长度0～8，镜像位置-1表示不检查";
  else if (c.scanEvery<60 || c.scanEvery>604800 || c.scanSeconds<1 || c.scanSeconds>120 || c.uploadEvery<60 || c.uploadEvery>1209600 || c.workSeconds<60 || c.workSeconds>600 || c.workSeconds<c.scanSeconds+30) e="扫描/上传/工作窗口超出允许范围（上传最大14天）";
  else if (c.apSeconds<60 || c.apSeconds>1800 || c.screenSeconds<5 || c.screenSeconds>600 || c.staleSeconds<60 || c.staleSeconds>31536000 || c.maintenanceBootSeconds<30 || c.maintenanceBootSeconds>300) e="维护/显示/过期提示时长不合法（复位维护窗口30～300秒）";
  else if(c.retryAttempts<1 || c.retryAttempts>5 || c.retryBaseSeconds<10 || c.retryBaseSeconds>3600 || c.retryMaxSeconds<c.retryBaseSeconds || c.retryMaxSeconds>7200 || c.connectSeconds<10 || c.connectSeconds>120 || c.restartBatches<1 || c.restartBatches>10) e="故障恢复参数超界：次数1～5，退避10～7200秒，连接10～120秒，重启批次1～10";
  else if (!std::isfinite(c.usableMah) || !std::isfinite(c.sleepMa) || !std::isfinite(c.activeMa) || !std::isfinite(c.modemMa) || c.usableMah<100 || c.usableMah>10000 || c.sleepMa<0 || c.sleepMa>100 || c.activeMa<1 || c.activeMa>500 || c.modemMa<1 || c.modemMa>2000) e="电量模型超出范围（按5V输出侧建模）";
  else if (!master && c.fixed) { Frame f=c.fixedFrame; f.mac=c.targetMac; if (!frameValid(f,false,e)) return false; }
  return e.empty();
}
void configJson(const Config& c, JsonDocument& d, bool secrets) {
  d.clear();
#define OUT(k) d[#k]=c.k
  OUT(schema); OUT(generation); OUT(revision); OUT(deviceId); OUT(masterId); OUT(prefix); OUT(targetMac);
  OUT(wifiGeneration);OUT(mqttGeneration);
  if(secrets)OUT(appliedCommand);
  auto slots=d["slots"].to<JsonArray>();for(const auto& s:c.slots) {auto v=slots.add<JsonObject>();v["start"]=s.start;v["end"]=s.end;}
  OUT(wifiSsid); OUT(broker); OUT(mqttUser); OUT(caPem); OUT(apn); OUT(modemCa);
  OUT(tls); OUT(labPlaintext); OUT(fixed); OUT(sleep); OUT(batteryMode); OUT(strict); OUT(simulate);
  OUT(port); OUT(rollOffset); OUT(rollLength); OUT(rollMirror); OUT(scanEvery); OUT(scanSeconds);
  OUT(uploadEvery); OUT(workSeconds); OUT(apSeconds); OUT(screenSeconds); OUT(staleSeconds);
  OUT(usableMah); OUT(sleepMa); OUT(activeMa); OUT(modemMa);
  OUT(displayLock); OUT(recoveryRestart); OUT(retryAttempts); OUT(retryBaseSeconds); OUT(retryMaxSeconds);
  OUT(connectSeconds); OUT(restartBatches); OUT(maintenanceBootSeconds);
  if (secrets) { OUT(wifiPass); OUT(mqttPass); OUT(apPass); OUT(adminPass); }
#undef OUT
  d["fixedAdv"]=hex(c.fixedFrame.adv); d["fixedRsp"]=hex(c.fixedFrame.rsp); d["fixedPdu"]=c.fixedFrame.pdu;
  if (!secrets) { d["wifiPasswordSet"]=!c.wifiPass.empty(); d["mqttPasswordSet"]=!c.mqttPass.empty(); }
}
bool configPatch(Config& c, JsonObjectConst d, bool master, std::string& e) {
  Config n=c;
  for (JsonPairConst p:d) {
    std::string key=p.key().c_str(); JsonVariantConst v=p.value(); bool known=false;
#define STR(k) if(key==#k) { known=true; if(!v.is<std::string>()) {e="字符串类型错误: "+key;return false;} n.k=v.as<std::string>(); }
#define NUM(k,T) if(key==#k) { known=true; if(!v.is<T>()) {e="数字类型错误: "+key;return false;} n.k=v.as<T>(); }
#define BOOL(k) NUM(k,bool)
    STR(deviceId) STR(masterId) STR(prefix) STR(targetMac) STR(wifiSsid) STR(broker) STR(mqttUser) STR(caPem) STR(apn) STR(modemCa)
    BOOL(tls) BOOL(labPlaintext) BOOL(fixed) BOOL(sleep) BOOL(batteryMode) BOOL(strict) BOOL(simulate)
    BOOL(displayLock) BOOL(recoveryRestart)
    NUM(retryAttempts,uint32_t) NUM(retryBaseSeconds,uint32_t) NUM(retryMaxSeconds,uint32_t)
    NUM(cooldownSeconds,uint32_t) NUM(connectSeconds,uint32_t) NUM(restartBatches,uint32_t)
    NUM(schema,uint32_t) NUM(generation,uint32_t) NUM(revision,uint32_t) NUM(port,int) NUM(rollOffset,int) NUM(rollLength,int) NUM(rollMirror,int)
    NUM(wifiGeneration,uint32_t) NUM(mqttGeneration,uint32_t)
    NUM(scanEvery,uint32_t) NUM(scanSeconds,uint32_t) NUM(uploadEvery,uint32_t) NUM(workSeconds,uint32_t)
    NUM(apSeconds,uint32_t) NUM(screenSeconds,uint32_t) NUM(staleSeconds,uint32_t) NUM(maintenanceBootSeconds,uint32_t)
    NUM(usableMah,double) NUM(sleepMa,double) NUM(activeMa,double) NUM(modemMa,double)
#undef STR
#undef NUM
#undef BOOL
    if(key=="wifiPass" || key=="mqttPass" || key=="apPass" || key=="adminPass") {
      known=true; if(!v.is<std::string>()) { e="密码类型错误"; return false; }
      auto s=v.as<std::string>(); if(!s.empty()) { if(key=="wifiPass") n.wifiPass=s; if(key=="mqttPass") n.mqttPass=s; if(key=="apPass") n.apPass=s; if(key=="adminPass") n.adminPass=s; }
    }
    if(key=="clearWifiPass" || key=="clearMqttPass") {
      known=true; if(!v.is<bool>()) {e="清空标记应为布尔值";return false;}
      if(v.as<bool>()) { if(key=="clearWifiPass") n.wifiPass.clear(); else n.mqttPass.clear(); }
    }
    if(key=="fixedAdv" || key=="fixedRsp") {
      known=true; if(!v.is<std::string>() || !unhex(v.as<std::string>(),key=="fixedAdv"?n.fixedFrame.adv:n.fixedFrame.rsp)) {e="固定 raw 非法十六进制或超过31字节";return false;}
    }
    if(key=="fixedPdu") {known=true; if(!v.is<std::string>()) {e="PDU 类型错误";return false;} n.fixedFrame.pdu=v.as<std::string>();}
    if(key=="appliedCommand") {known=true;if(!v.is<std::string>()) {e="命令编号非法";return false;}n.appliedCommand=v.as<std::string>();}
    if(key=="slots") {
      known=true;if(!v.is<JsonArrayConst>() || v.size()>5) {e="工作时段应为最多5项的数组";return false;}
      n.slots.clear();for(auto item:v.as<JsonArrayConst>()) {
        if(!item.is<JsonObjectConst>() || item.size()!=2 || !item["start"].is<uint16_t>() || !item["end"].is<uint16_t>()) {e="时段需start/end分钟数";return false;}
        n.slots.push_back({item["start"].as<uint16_t>(),item["end"].as<uint16_t>()});
      }
    }
    // Read-only UI metadata is never accepted in a write request.
    if(!known) {e="未知配置项: "+key;return false;}
  }
  n.targetMac=upper(n.targetMac);
  bool sameSource=n.masterId==c.masterId && n.prefix==c.prefix && n.targetMac==c.targetMac;
  // Loading an existing config into an unbound default, and a slave selecting
  // another source, must remain legal. Only the same master's era is monotonic.
  if(master && sameSource && n.generation<c.generation) {e="同一主端绑定的数据代次 generation 不可回退";return false;}
  if(master && n.targetMac!=c.targetMac && !c.targetMac.empty() && n.generation<=c.generation) {e="更换目标须提高数据代次 generation，并同步从端";return false;}
  n.fixedFrame.mac=n.targetMac;
  if(!configValid(n,master,e)) return false;
  n.cooldownSeconds=0; // Legacy input is consumed but never re-enabled.
  c=n; return true;
}
bool advanceNetworkConfig(const Config& before,Config& after,std::string& error) {
  bool wifi=before.wifiSsid!=after.wifiSsid || before.wifiPass!=after.wifiPass;
  bool mqtt=before.broker!=after.broker || before.port!=after.port || before.mqttUser!=after.mqttUser || before.mqttPass!=after.mqttPass || before.caPem!=after.caPem || before.tls!=after.tls || before.apn!=after.apn || before.modemCa!=after.modemCa;
  if((wifi && before.wifiGeneration==UINT32_MAX) || (mqtt && before.mqttGeneration==UINT32_MAX)) {error="网络配置代次已耗尽";return false;}
  after.wifiGeneration=before.wifiGeneration+(wifi?1:0);after.mqttGeneration=before.mqttGeneration+(mqtt?1:0);return true;
}
void messageJson(const Message& m, JsonDocument& d, int battery) {
  d.clear();
  d["v"]=1; d["device_id"]=m.master; d["generation"]=m.generation; d["mac"]=m.frame.mac;
  d["addr_type"]="random_static"; d["adv_type"]=m.frame.pdu;
  d["adv_hex"]=hex(m.frame.adv); d["scan_rsp_hex"]=hex(m.frame.rsp);
  d["observed_at"]=m.observed; d["changed_at"]=m.changed; d["reported_at"]=m.reported;
  d["time_valid"]=m.timeValid; d["seq"]=m.seq; d["rssi"]=m.rssi;
  if(battery<0) d["battery_est_pct"]=nullptr; else d["battery_est_pct"]=battery;
  d["battery_estimated"]=true;
}
std::string encodeMessage(const Message& m, int battery) {JsonDocument d;messageJson(m,d,battery);std::string s;serializeJson(d,s);return s;}
bool decodeMessage(const std::string& s, Message& m, std::string& e) {
  if(s.empty() || s.size()>MaxPayload) {e="MQTT 报文超限";return false;}
  JsonDocument d; if(deserializeJson(d,s,DeserializationOption::NestingLimit(4)) || !d.is<JsonObject>()) {e="MQTT JSON 非法";return false;}
  if(!d["v"].is<int>() || d["v"].as<int>()!=1 || !d["generation"].is<uint32_t>() || !d["seq"].is<uint64_t>() || !d["time_valid"].is<bool>() || !d["rssi"].is<int>()) {e="协议版本/代次/序号/时间标记非法";return false;}
  for(auto key:{"device_id","mac","addr_type","adv_type","adv_hex","scan_rsp_hex"}) if(!d[key].is<std::string>()) {e="缺少字符串字段";return false;}
  for(auto key:{"observed_at","changed_at","reported_at"}) if(!d[key].is<uint64_t>()) {e="时间字段类型非法";return false;}
  Message n;
  n.master=d["device_id"].as<std::string>(); n.generation=d["generation"].as<uint32_t>(); n.seq=d["seq"].as<uint64_t>();
  n.frame.mac=upper(d["mac"].as<std::string>()); n.frame.pdu=d["adv_type"].as<std::string>();
  if(d["addr_type"].as<std::string>()!="random_static" || !unhex(d["adv_hex"],n.frame.adv) || !unhex(d["scan_rsp_hex"],n.frame.rsp) || !frameValid(n.frame,false,e)) {if(e.empty())e="地址类型或 hex 非法";return false;}
  n.observed=d["observed_at"].as<uint64_t>(); n.changed=d["changed_at"].as<uint64_t>(); n.reported=d["reported_at"].as<uint64_t>();
  n.timeValid=d["time_valid"].as<bool>(); n.rssi=d["rssi"].as<int>();
  if(!safeToken(n.master,32) || !n.generation || !n.seq || n.rssi < -127 || n.rssi > 20 || n.observed>4102444800ULL || n.changed>4102444800ULL || n.reported>4102444800ULL || (!n.timeValid && n.reported) || (n.timeValid && n.reported<1704067200ULL) || (n.observed && n.changed>n.observed)) {e="消息元数据超界或时间矛盾";return false;}
  m=n; return true;
}
std::string topic(const Config& c, const char* leaf) {return "btclone/"+c.prefix+"/"+c.masterId+"/"+leaf;}
}
