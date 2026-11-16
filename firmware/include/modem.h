#pragma once
#include <Arduino.h>
#include "protocol.h"
#include <deque>

// Single owner of UART1. All operations are pumped from loop(); web actions
// enqueue requests and never issue concurrent AT commands.
class Modem {
 public:
  void begin();
  bool start(const bt::Config& config, bool networkOnly=false,bool control=false);
  void tick();
  bool publish(const std::string& topic,const std::string& json,bool retain=true);
  bool pop(std::string& topic,std::string& payload);
  bool syncClock();
  void shutdown();
  void prepareSleep();
  bool busy() const;
  bool ready() const {return state_==Ready;}
  bool registered=false, connected=false, published=false, clockValid=false,subscribed=false;
  int rssi=-127;
  bool shutdownConfirmed=false;
  bool shutdownAttempted=false;
  uint8_t shutdownAttempts=0;
  std::string shutdownError;
  bool networkPassed=false;
  uint64_t epoch=0;
  std::string status="未启动", identity;
  int failureStep=-1, errorCode=-1, connectionCode=-1;
  std::string networkClock;
  std::string simStatus="未检查";
  uint32_t receivedBytes=0;
  std::string lastReadShape,lastReadFailureShape,lastEmptyReadShape;
  uint32_t readFailures=0;
 private:
  enum State {Idle,Probe,Pulse,BootWait,Setup,SimWait,ClockZone,ClearSession,Ready,Publish,ReadCheck,ReadMessage,Signal,SyncClock,PowerOff,OffWait,Done};
  State state_=Idle;
  HardwareSerial uart_{1};
  bt::Config cfg_;
  String line_,response_;
  std::string payload_;
  bt::Ack ack_;
  uint32_t sentAt_=0,started_=0,timeout_=0;
  uint32_t bootAt_=0;
  bool bootProbe_=false;
  // After observing a live modem, allow another key pulse only following an
  // actual shutdown confirmation. A failed AT transaction is not power-off.
  uint8_t probeMisses_=0,simRetries_=0;
  bool powerPulseDone_=false,powerSeenOn_=false,probeResponded_=false;
  bool simBlocked_=false;
  int step_=0;
  bool ok_=false,error_=false,sim_=false,netOnly_=false,prompt_=false,discard_=false;
  bool clockSeen_=false;
  bool clockOnly_=false;
  bool disconnectSeen_=false;
  bool control_=false,readPending_=false;
  int subMid_=-1,subAckMid_=-1,subCode_=-1;
  uint32_t lastRead_=0,lastSignal_=0;
  std::deque<std::pair<std::string,std::string>> inbox_;
  void command(const String& text,uint32_t timeout=5000,const std::string& data="");
  void receive();
  void next();
  void fail(const char* reason);
  void startClock(bool online);
};
