#include "remote.h"
#include <algorithm>
#include <cstdlib>

namespace bt {
std::string shortText(const std::string& input,size_t bytes) {
  size_t n=std::min(input.size(),bytes);
  while(n>0 && n<input.size() && (static_cast<unsigned char>(input[n])&0xC0)==0x80)--n;
  return input.substr(0,n);
}
bool jsonObject(const std::string& input,JsonDocument& d,uint8_t depth) {
  struct Reader {
    const std::string& text;size_t at=0;
    int read(){return at<text.size()?static_cast<unsigned char>(text[at++]):-1;}
    size_t readBytes(char* out,size_t length){size_t n=std::min(length,text.size()-at);memcpy(out,text.data()+at,n);at+=n;return n;}
  } reader{input};
  if(deserializeJson(d,reader,DeserializationOption::NestingLimit(depth)) || !d.is<JsonObject>())return false;
  while(reader.at<input.size() && (input[reader.at]==' ' || input[reader.at]=='\t' || input[reader.at]=='\r' || input[reader.at]=='\n'))++reader.at;
  return reader.at==input.size();
}
bool slotsValid(const std::vector<WorkSlot>& slots,std::string& error) {
  if(slots.size()>MaxSlots) {error="每天最多5个时段";return false;}
  for(size_t i=0;i<slots.size();++i) {
    const auto& s=slots[i];
    if(s.start>=1440 || s.end>1440 || s.start>=s.end) {error="时段须同日开始早于结束，不支持跨午夜";return false;}
    for(size_t j=0;j<i;++j)if(s.start<slots[j].end && slots[j].start<s.end) {error="工作时段不能重叠";return false;}
  }
  return true;
}
Window workWindow(const Config& c,uint64_t now,const std::vector<uint64_t>& completed) {
  Window out;if(!now)return out;
  uint64_t midnight=((now+28800)/86400)*86400-28800;
  for(const auto& s:c.slots)for(int day=0;day<2;++day) {
    uint64_t start=midnight+day*86400+s.start*60,end=midnight+day*86400+s.end*60;
    if(std::find(completed.begin(),completed.end(),start)!=completed.end())continue;
    if(now>=start && now<end) {out.start=start;out.end=end;}
    if(start>now && (!out.next || start<out.next))out.next=start;
  }
  return out;
}
std::string deviceTopic(const Config& c,const std::string& leaf) {return topic(c,("devices/"+c.deviceId+"/"+leaf).c_str());}
bool remotePatch(Config& c,JsonObjectConst input,bool master,std::string& error) {
  const std::string allowed="|sleep|slots|scanEvery|scanSeconds|uploadEvery|workSeconds|screenSeconds|maintenanceBootSeconds|staleSeconds|retryAttempts|retryBaseSeconds|retryMaxSeconds|connectSeconds|recoveryRestart|restartBatches|batteryMode|usableMah|sleepMa|activeMa|modemMa|fixed|fixedAdv|fixedRsp|fixedPdu|simulate|rollOffset|rollLength|rollMirror|strict|targetMac|generation|";
  for(JsonPairConst p:input)if(allowed.find("|"+std::string(p.key().c_str())+"|")==std::string::npos) {error="该参数仅允许本地维护修改";return false;}
  return configPatch(c,input,master,error);
}
bool decodeCommand(const Config& c,const std::string& t,const std::string& payload,uint64_t now,bool master,Command& out,std::string& error) {
  if(payload.empty() || payload.size()>MaxPayload) {error="命令为空或超长";return false;}
  JsonDocument d;if(!jsonObject(payload,d)) {error="命令JSON非法";return false;}
  if(!d["v"].is<int>() || d["v"].as<int>()!=1 || !d["id"].is<std::string>() || !d["action"].is<std::string>() || !d["device_id"].is<std::string>() || d["device_id"].as<std::string>()!=c.deviceId || !d["issued_at"].is<uint64_t>() || !d["expires_at"].is<uint64_t>()) {error="命令信封或设备编号非法";return false;}
  Command n;n.id=d["id"].as<std::string>();n.action=d["action"].as<std::string>();n.issued=d["issued_at"].as<uint64_t>();n.expires=d["expires_at"].as<uint64_t>();
  if(!safeToken(n.id,40) || (n.action!="config" && n.action!="restart" && !(master && (n.action=="run" || n.action=="battery-full")))) {error="不允许的命令";return false;}
  if(t!=deviceTopic(c,"cmd/"+n.action+"/"+n.id)) {error="命令主题与内容不一致";return false;}
  if(!n.issued || n.expires<=n.issued || n.expires-n.issued>7*86400 || (now && n.issued>now+300)) {error="命令时间或有效期非法（最多7天）";return false;}
  if(n.action=="config") {
    if(!d["expected_revision"].is<uint32_t>() || !d["patch"].is<JsonObject>()) {error="配置命令缺少版本或补丁";return false;}
    n.revision=d["expected_revision"].as<uint32_t>();serializeJson(d["patch"],n.patch);
  }
  for(JsonPairConst p:d.as<JsonObjectConst>()) {
    const std::string k=p.key().c_str();
    if(k!="v" && k!="id" && k!="action" && k!="device_id" && k!="issued_at" && k!="expires_at" && !(n.action=="config" && (k=="expected_revision" || k=="patch"))) {error="命令包含未知字段";return false;}
  }
  out=n;return true;
}
Receipt* RemoteState::find(const std::string& id) {for(auto& r:receipts)if(r.command.id==id)return &r;return nullptr;}
bool RemoteState::reserve(const Command& c,uint64_t now,std::string& error) {
  if(find(c.id)) {error="duplicate";return false;}
  while(receipts.size()>=ReceiptLimit) {
    auto it=std::find_if(receipts.begin(),receipts.end(),[&](const Receipt& r){return r.command.expires<now && r.state!="running" && r.state!="received";});
    if(it==receipts.end()) {error="命令记录已满，需等待旧命令过期";return false;}
    receipts.erase(it);
  }
  Receipt r;r.command=c;r.at=now;r.state=now>=c.expires?"expired":"received";
  receipts.push_back(r);
  std::string serialized;
  // Reserve space for a full history and terminal receipt reasons, so a burst
  // of large config commands cannot turn into an NVS "hardware" fault later.
  while(!encode(serialized) || serialized.size()>5500) {
    auto it=std::find_if(receipts.begin(),receipts.end(),[&](const Receipt& old){return old.command.id!=c.id && old.command.expires<now && old.state!="running" && old.state!="received";});
    if(it==receipts.end()) {receipts.pop_back();error="命令记录空间不足，请等待已收命令完成或过期";return false;}
    receipts.erase(it);
  }
  return true;
}
void RemoteState::record(uint64_t now,bool success,const std::string& reason,uint64_t window) {
  history.push_back({now,window,success,shortText(reason)});while(history.size()>HistoryLimit)history.pop_front();
}
void RemoteState::finishSlot(uint64_t now,bool success,const std::string& reason) {
  if(!slot)return;
  record(now,success,reason,slot);completed.push_back(slot);while(completed.size()>10)completed.erase(completed.begin());
  slot=slotEnd=retryAt=0;attempts=0;slotReason.clear();
}
bool RemoteState::encode(std::string& value) const {
  JsonDocument d;d["v"]=1;d["slot"]=slot;d["end"]=slotEnd;d["retry"]=retryAt;d["attempts"]=attempts;d["reason"]=slotReason;
  auto done=d["completed"].to<JsonArray>();for(auto s:completed)done.add(s);
  auto hist=d["history"].to<JsonArray>();for(const auto& h:history) {auto v=hist.add<JsonObject>();v["at"]=h.at;v["slot"]=h.slot;v["ok"]=h.success;v["reason"]=h.reason;}
  auto ack=d["receipts"].to<JsonArray>();for(const auto& r:receipts) {
    auto v=ack.add<JsonObject>();v["id"]=r.command.id;v["action"]=r.command.action;v["issued"]=r.command.issued;v["expires"]=r.command.expires;v["revision"]=r.command.revision;
    // Received commands survive power loss; executed command patches need not.
    if(r.state=="received")v["patch"]=r.command.patch;
    v["state"]=r.state;v["reason"]=r.reason;v["at"]=r.at;
  }
  value.clear();serializeJson(d,value);return value.size()<=9500;
}
bool RemoteState::decode(const std::string& value) {
  JsonDocument d;if(!jsonObject(value,d) || d["v"].as<int>()!=1)return false;
  RemoteState n;n.slot=d["slot"]|0ULL;n.slotEnd=d["end"]|0ULL;n.retryAt=d["retry"]|0ULL;n.attempts=d["attempts"]|0UL;n.slotReason=d["reason"]|"";
  for(auto s:d["completed"].as<JsonArrayConst>()) {if(!s.is<uint64_t>())return false;n.completed.push_back(s.as<uint64_t>());}
  for(auto v:d["history"].as<JsonArrayConst>())n.history.push_back({v["at"]|0ULL,v["slot"]|0ULL,v["ok"]|false,v["reason"]|""});
  for(auto v:d["receipts"].as<JsonArrayConst>()) {
    Receipt r;r.command.id=v["id"]|"";r.command.action=v["action"]|"";r.command.issued=v["issued"]|0ULL;r.command.expires=v["expires"]|0ULL;r.command.revision=v["revision"]|0UL;r.command.patch=v["patch"]|"";
    r.state=v["state"]|"";r.reason=v["reason"]|"";r.at=v["at"]|0ULL;if(!safeToken(r.command.id,40))return false;n.receipts.push_back(r);
  }
  if(n.completed.size()>10 || n.history.size()>HistoryLimit || n.receipts.size()>ReceiptLimit || n.attempts>5)return false;
  *this=n;return true;
}
// Tokenizer accepts quoted topic/hex and unquoted numeric fields. No escapes,
// commas or control characters are permitted in MQTT topics used by firmware.
static bool token(const std::string& s,size_t& p,std::string& out) {
  while(p<s.size() && s[p]==' ')++p;
  if(p>=s.size())return false;
  if(s[p]=='"') {size_t e=s.find('"',++p);if(e==std::string::npos)return false;out=s.substr(p,e-p);p=e+1;}
  else {size_t e=s.find(',',p);if(e==std::string::npos)e=s.size();out=s.substr(p,e-p);p=e;while(!out.empty() && out.back()==' ')out.pop_back();}
  while(p<s.size() && s[p]==' ')++p;
  if(p<s.size()) {if(s[p++]!=',')return false;}
  return true;
}
bool parseModemMessage(const std::string& line,std::string& t,std::string& payload) {
  if(line.find("+MQTTREAD:")!=0)return false;
  size_t p=10;std::string cid,mid,topic,length,data;
  if(!token(line,p,cid) || !token(line,p,mid) || !token(line,p,topic) || !token(line,p,length) || cid!="0" || mid.empty() || length.empty())return false;
  for(char c:mid)if(c<'0'||c>'9')return false;
  for(char c:length)if(c<'0'||c>'9')return false;
  if(topic.empty() || topic.size()>256 || topic.find_first_of("\"\r\n,+#")!=std::string::npos)return false;
  unsigned long len=strtoul(length.c_str(),nullptr,10);std::vector<uint8_t> decoded;
  // Cached retained-message deletions may omit the empty data field entirely.
  // Still validate the topic and declared length; a nonempty missing body fails.
  if(p<line.size()) {if(!token(line,p,data) || p!=line.size())return false;}
  else if(len!=0)return false;
  if(len>MaxPayload || data.size()!=len*2 || !unhex(data,decoded,MaxPayload))return false;
  t=topic;payload.assign(decoded.begin(),decoded.end());return true;
}
}
