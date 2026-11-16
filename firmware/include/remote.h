#pragma once
#include "protocol.h"
#include <deque>

namespace bt {
bool jsonObject(const std::string& input,JsonDocument& document,uint8_t depth=5);
std::string shortText(const std::string& input,size_t bytes=96);
constexpr size_t MaxSlots=5, HistoryLimit=12, ReceiptLimit=16;
struct Window {
  uint64_t start=0,end=0,next=0;
  bool active() const {return start!=0;}
};
Window workWindow(const Config& config,uint64_t now,const std::vector<uint64_t>& completed={});
bool slotsValid(const std::vector<WorkSlot>& slots,std::string& error);
std::string deviceTopic(const Config& config,const std::string& leaf);
bool remotePatch(Config& config,JsonObjectConst input,bool master,std::string& error);
struct Command {
  std::string id,action;
  uint64_t issued=0,expires=0;
  uint32_t revision=0;
  std::string patch;
};
bool decodeCommand(const Config& config,const std::string& topic,const std::string& payload,
                   uint64_t now,bool master,Command& result,std::string& error);
struct Receipt {
  Command command;
  std::string state="received",reason;
  uint64_t at=0;
};
struct History {
  uint64_t at=0,slot=0;
  bool success=false;
  std::string reason;
};
// Kept separately from raw snapshots. Persist before any remote side effect.
struct RemoteState {
  std::vector<uint64_t> completed;
  uint64_t slot=0,slotEnd=0,retryAt=0;
  uint32_t attempts=0;
  std::string slotReason;
  std::deque<History> history;
  std::deque<Receipt> receipts;
  Receipt* find(const std::string& id);
  bool reserve(const Command& command,uint64_t now,std::string& error);
  void finishSlot(uint64_t now,bool success,const std::string& reason);
  void record(uint64_t now,bool success,const std::string& reason,uint64_t window=0);
  bool encode(std::string& value) const;
  bool decode(const std::string& value);
};
// Strict ML307 downlink, hexadecimal output, cached mode (manual pp. 19/28).
bool parseModemMessage(const std::string& line,std::string& topic,std::string& payload);
}
