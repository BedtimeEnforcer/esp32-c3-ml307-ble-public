#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string>
#include <vector>

namespace bt {
constexpr size_t MaxPayload = 1024;
struct Frame {
  std::string mac;
  std::vector<uint8_t> adv, rsp;
  std::string pdu = "ADV_IND";
  bool operator==(const Frame& b) const { return mac == b.mac && adv == b.adv && rsp == b.rsp && pdu == b.pdu; }
};
struct Message {
  Frame frame;
  std::string master;
  uint32_t generation = 1;
  uint64_t seq = 0, observed = 0, changed = 0, reported = 0;
  bool timeValid = false;
  int rssi = 0;
};
bool macValid(const std::string& mac);
bool staticMac(const std::string& mac);
bool unhex(const std::string& text, std::vector<uint8_t>& out, size_t maxBytes = 31);
std::string hex(const std::vector<uint8_t>& value);
bool adValid(const std::vector<uint8_t>& data);
bool frameValid(const Frame& frame, bool strict, std::string& error);
std::string rolling(const Frame& frame, size_t offset, size_t length, int mirror, bool& matches);
bool safeToken(const std::string& text, size_t maximum);
bool atSafe(const std::string& text, size_t maximum);
enum class Freshness { New, Duplicate, Reject };
Freshness freshness(const Message& incoming, const Message* old, const std::string& master,
                    const std::string& mac, uint32_t generation);
struct Ack {
  int connection = -1, messageId = -1, ackConnection = -1, ackId = -1;
  void reset() { *this = Ack{}; }
  bool line(const std::string& text);
  bool complete() const { return connection == 0 && messageId >= 0 && ackConnection == connection && ackId == messageId; }
};
struct Battery {
  double usedMah = 0;
  bool calibrated = false;
  void charge(double ma, double seconds) { if (seconds > 0 && ma >= 0) usedMah += ma * seconds / 3600.0; }
  int percent(double usableMah) const;
};
bool parseClock(const std::string& line, uint64_t& epoch);
uint32_t checksum(const std::string& value);
uint32_t until(uint32_t now, uint32_t deadline);
uint32_t wakeDelay(uint32_t now, uint32_t scanDeadline, uint32_t uploadDeadline);
}
