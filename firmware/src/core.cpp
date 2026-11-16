#include "core.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cmath>

namespace bt {
static int digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}
bool macValid(const std::string& s) {
  if (s.size() != 17) return false;
  for (size_t i = 0; i < 17; ++i) if (i % 3 == 2 ? s[i] != ':' : digit(s[i]) < 0) return false;
  return true;
}
bool staticMac(const std::string& s) {
  return macValid(s) && (digit(s[0]) & 12) == 12 && s != "FF:FF:FF:FF:FF:FF" && s != "C0:00:00:00:00:00";
}
bool unhex(const std::string& s, std::vector<uint8_t>& out, size_t limit) {
  if (s.size() % 2 || s.size() / 2 > limit) return false;
  std::vector<uint8_t> result;
  for (size_t i = 0; i < s.size(); i += 2) {
    int a = digit(s[i]), b = digit(s[i + 1]);
    if (a < 0 || b < 0) return false;
    result.push_back((a << 4) | b);
  }
  out.swap(result); return true;
}
std::string hex(const std::vector<uint8_t>& data) {
  const char* digits = "0123456789ABCDEF";
  std::string s;
  for (auto b : data) { s += digits[b >> 4]; s += digits[b & 15]; }
  return s;
}
bool adValid(const std::vector<uint8_t>& d) {
  if (d.size() > 31) return false;
  for (size_t i = 0; i < d.size();) {
    if (!d[i] || i + 1 + d[i] > d.size()) return false;
    i += d[i] + 1;
  }
  return true;
}
static std::vector<uint8_t> field(const std::vector<uint8_t>& d, uint8_t type) {
  for (size_t i = 0; i < d.size(); i += d[i] + 1)
    if (d[i + 1] == type) return {d.begin() + i + 2, d.begin() + i + d[i] + 1};
  return {};
}
bool frameValid(const Frame& f, bool strict, std::string& error) {
  error.clear();
  if (!staticMac(f.mac)) error = "仅支持已确认的随机静态 MAC";
  else if (f.adv.empty() || !adValid(f.adv) || !adValid(f.rsp)) error = "ADV/RSP 长度或 AD 结构不合法";
  else if (f.pdu != "ADV_IND" && f.pdu != "ADV_SCAN_IND" && f.pdu != "ADV_NONCONN_IND") error = "不支持的 PDU 类型";
  else if (f.pdu == "ADV_NONCONN_IND" && !f.rsp.empty()) error = "不可扫描广播不能携带 RSP";
  else if (strict) {
    auto m = field(f.adv, 0xFF), s = field(f.rsp, 0x16), n = field(f.rsp, 0x09);
    if (f.rsp.empty()) error = "已找到目标，但缺少扫描响应";
    else if (m.size() != 25 || m[0] != 0x4C || m[1] != 0 || m[2] != 2 || m[3] != 0x15 ||
             s.size() != 13 || s[0] != 0xA5 || s[1] != 0xFD ||
             std::string(n.begin(), n.end()) != "MBeacon") error = "目标格式不符合原 BLE 完整性规则";
  }
  return error.empty();
}
std::string rolling(const Frame& f, size_t off, size_t count, int mirror, bool& matches) {
  auto raw = f.adv; raw.insert(raw.end(), f.rsp.begin(), f.rsp.end());
  matches = false;
  if (!count || count > 8 || off + count > raw.size()) return "";
  matches = mirror < 0 || (static_cast<size_t>(mirror) + count <= raw.size() &&
    std::equal(raw.begin() + off, raw.begin() + off + count, raw.begin() + mirror));
  return hex({raw.begin() + off, raw.begin() + off + count});
}
bool safeToken(const std::string& s, size_t max) {
  if (s.empty() || s.size() > max) return false;
  for (unsigned char c : s) if (!std::isalnum(c) && c != '-' && c != '_') return false;
  return true;
}
bool atSafe(const std::string& s, size_t max) {
  if (s.size() > max) return false;
  for (unsigned char c : s) if (c < 32 || c == 127 || c == '"' || c == '\\') return false;
  return true;
}
Freshness freshness(const Message& n, const Message* old, const std::string& master, const std::string& mac, uint32_t gen) {
  if (n.master != master || n.frame.mac != mac || n.generation != gen || !n.seq) return Freshness::Reject;
  if (!old) return Freshness::New;
  if (n.seq < old->seq) return Freshness::Reject;
  if (n.seq == old->seq) return n.frame == old->frame && n.observed == old->observed && n.changed == old->changed ? Freshness::Duplicate : Freshness::Reject;
  if (old->observed && n.observed && n.observed < old->observed) return Freshness::Reject;
  return Freshness::New;
}
bool Ack::line(const std::string& s) {
  int a, b, c, end = 0;
  if (std::sscanf(s.c_str(), "+MQTTPUB: %d,%d,%d%n", &a,&b,&c,&end) == 3 && end == static_cast<int>(s.size()) && a == 0 && b >= 0 && b <= 65535 && c > 0) { connection = a; messageId = b; }
  end = 0;
  if (std::sscanf(s.c_str(), "+MQTTURC: \"puback\",%d,%d,%d%n", &a,&b,&c,&end) == 3 && end == static_cast<int>(s.size()) && a == 0 && b >= 0 && b <= 65535 && (c == 0 || c == 1)) { ackConnection = a; ackId = b; }
  return complete();
}
int Battery::percent(double capacity) const {
  if (!calibrated || capacity <= 0 || !std::isfinite(usedMah)) return -1;
  return std::max(0, std::min(100, static_cast<int>(std::round(100 * (1 - usedMah / capacity)))));
}
// Civil date conversion avoids dependence on host/device time zone.
bool parseClock(const std::string& s, uint64_t& epoch) {
  // ML307 manuals specify yy/MM/dd,hh:mm:ss+zz, with zz in quarter hours.
  // Consume the entire quoted value; never accept a prefix of +08:00 or junk.
  size_t p=6;
  bool ntp=s.compare(0,6,"+MNTP:")==0;
  if(!ntp && s.compare(0,6,"+CCLK:")!=0)return false;
  while(p<s.size() && s[p]==' ')++p;
  if(ntp) {if(s.compare(p,2,"0,")!=0)return false;p+=2;while(p<s.size() && s[p]==' ')++p;}
  if(s.size()!=p+22 || s[p]!='"' || s[p+21]!='"')return false;
  const char* t=s.c_str()+p+1;
  if(t[2]!='/' || t[5]!='/' || t[8]!=',' || t[11]!=':' || t[14]!=':' || (t[17]!='+' && t[17]!='-'))return false;
  for(int at:{0,3,6,9,12,15,18})if(t[at]<'0' || t[at]>'9' || t[at+1]<'0' || t[at+1]>'9')return false;
  auto pair=[&](int at){return (t[at]-'0')*10+t[at+1]-'0';};
  int y=pair(0),m=pair(3),d=pair(6),h=pair(9),min=pair(12),sec=pair(15),zone=pair(18);char sign=t[17];
  y += 2000;
  bool leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
  const int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  if (y < 2024 || y > 2099 || m < 1 || m > 12 || d < 1 || d > days[m-1]+(m==2&&leap) || h>23 || min>59 || sec>59 || h<0 || min<0 || sec<0 || zone>56 || zone<0 || (sign!='+' && sign!='-')) return false;
  int year = y - (m <= 2), era = year / 400, yo = year - era*400;
  unsigned doy = (153*(m+(m>2?-3:9))+2)/5+d-1;
  int64_t day = era*146097+yo*365+yo/4-yo/100+doy-719468;
  epoch = day*86400+h*3600+min*60+sec-(sign=='+'?1:-1)*zone*900;
  return true;
}
uint32_t checksum(const std::string& s) { uint32_t h=2166136261UL; for (unsigned char c:s) h=(h^c)*16777619UL; return h; }
uint32_t until(uint32_t now,uint32_t deadline) {int32_t delta=static_cast<int32_t>(deadline-now);return delta>0?static_cast<uint32_t>(delta):0;}
uint32_t wakeDelay(uint32_t now,uint32_t scan,uint32_t upload) {return std::min(until(now,scan),until(now,upload));}
}
