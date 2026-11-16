#pragma once
#include "protocol.h"
#include <algorithm>

namespace bt {
constexpr uint32_t DaySeconds=86400;
inline bool reached(uint32_t now,uint32_t deadline) {return int32_t(now-deadline)>=0;}
enum class BootKind { Timer, PowerOn, ResetButton, RequestedRestart, AutomaticRestart, Fault };
class MaintenanceWindow {
 public:
  void begin(BootKind kind,uint32_t seconds,uint32_t now) {
    cancel();
    if(kind==BootKind::PowerOn || kind==BootKind::ResetButton || kind==BootKind::RequestedRestart)touch(seconds,now);
  }
  void touch(uint32_t seconds,uint32_t now) {enabled_=seconds!=0;deadline_=now+seconds*1000UL;}
  bool active(uint32_t now) const {return enabled_ && !reached(now,deadline_);}
  uint32_t remaining(uint32_t now) const {return active(now)?(deadline_-now+999)/1000:0;}
  void cancel() {enabled_=false;deadline_=0;}
 private:
  bool enabled_=false;
  uint32_t deadline_=0;
};
// Both the active deadline and its epoch equivalent are persisted by the owner.
// Restored waits are capped by short retry backoff; no separate batch cooldown.
struct Recovery {
  uint32_t attempts=0,batches=0,deadline=0,waitSeconds=0;
  uint64_t retryEpoch=0;
  bool waiting=false;
  bool allowed(uint32_t now) const {return !waiting || reached(now,deadline);}
  uint32_t remaining(uint32_t now) const {return allowed(now)?0:(deadline-now+999)/1000;}
  void defer(uint32_t seconds,uint32_t now,uint64_t epoch) {
    waitSeconds=std::max(seconds,remaining(now));
    deadline=now+waitSeconds*1000;waiting=true;retryEpoch=epoch?epoch+waitSeconds:0;
  }
  bool fail(const Config& c,uint32_t now,uint64_t epoch) {
    ++attempts;
    bool exhausted=attempts>=c.retryAttempts;
    waitSeconds=std::min<uint32_t>(c.retryMaxSeconds,c.retryBaseSeconds*(1UL<<std::min<uint32_t>(attempts-1,5)));
    if(exhausted) {attempts=0;batches=std::min<uint32_t>(batches+1,1000000);}
    deadline=now+waitSeconds*1000;waiting=true;retryEpoch=epoch?epoch+waitSeconds:0;
    return exhausted;
  }
  bool success() {bool changed=waiting || attempts || batches;*this=Recovery{};return changed;}
  void restore(const Config& c,uint32_t now,uint64_t epoch,uint32_t savedAttempts,uint32_t savedBatches,uint64_t savedRetry,bool savedWaiting) {
    attempts=std::min(savedAttempts,c.retryAttempts-1);batches=std::min<uint32_t>(savedBatches,1000000);
    if(!savedWaiting)return;
    waiting=true;retryEpoch=savedRetry;
    waitSeconds=epoch && savedRetry ? uint32_t(std::min<uint64_t>(savedRetry>epoch?savedRetry-epoch:0,c.retryMaxSeconds)):c.retryBaseSeconds;
    deadline=now+waitSeconds*1000;
  }
};
inline uint32_t deferredUpload(uint32_t now,const Recovery& r) {
  return r.allowed(now)?now+10000:r.deadline;
}
inline bool syncDue(uint64_t now,uint64_t last) {return !now || !last || now<last || now-last>=DaySeconds;}
inline uint32_t syncDelay(uint64_t now,uint64_t last) {return syncDue(now,last)?0:uint32_t(DaySeconds-(now-last));}
inline bool mayRestart(const Config& c,uint32_t batches,uint64_t now,uint64_t last) {
  return c.recoveryRestart && batches>=c.restartBatches && now && (!last || (now>=last && now-last>=DaySeconds));
}

// No visual feedback is returned while locked. Tokens are classified on release.
// Short: 50..600ms; hold: 800..1800ms. Other durations reset the sequence.
class AccessLock {
 public:
  void configure(bool enabled) {enabled_=enabled;relock();}
  void relock() {unlocked_=!enabled_;index_=0;started_=0;page=0;}
  bool locked() const {return !unlocked_;}
  bool release(uint32_t duration,uint32_t now) {
    if(unlocked_)return false;
    if(index_ && uint32_t(now-started_)>10000)index_=0;
    int token=duration>=50 && duration<=600?0:duration>=800 && duration<=1800?1:-1;
    static constexpr int pattern[]={0,0,1,0,0,1};
    if(token<0) {index_=0;return false;}
    if(token!=pattern[index_]) {index_=0;return false;}
    if(!index_)started_=now-duration; // Includes first press, not only release.
    if(uint32_t(now-started_)>10000) {index_=0;return false;}
    if(++index_==6) {unlocked_=true;index_=0;page=1;return true;}
    return false;
  }
  uint8_t page=0;
 private:
  bool enabled_=false,unlocked_=true;
  unsigned index_=0;
  uint32_t started_=0;
};
}
