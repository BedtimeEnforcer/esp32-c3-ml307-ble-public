#include "modem.h"
#include "persistence.h"
#include <esp_partition.h>
#include <nvs_flash.h>
#include <sys/time.h>
#include <iostream>
static int checks=0;
#define CHECK(x) do {++checks;if(!(x)){std::cerr<<"FAIL driver "<<__LINE__<<": "<<#x<<"\n";return -1;}} while(0)
static void resetFake(){fakeMillis=1;serialRx.clear();serialCommands.clear();serialPayloads.clear();pendingCommand.clear();fakeSetTimeCalls=fakeSetTimeResult=0;fakeSetTimeEpoch=0;}
static void silentProbes(Modem& modem){for(int i=0;i<12;++i){fakeMillis+=1601;modem.tick();}}
static void reachSim(Modem& modem,const bt::Config& c){modem.begin();modem.start(c);feed("OK\r\n");modem.tick();feed("OK\r\n");modem.tick();feed("ML307C\r\nOK\r\n");modem.tick();}
static bool advance(Modem& modem,bool wantAck=true,bool fragmented=false,bool goodTime=true) {
  size_t processed=0;
  for(int i=0;i<400;++i) {
    modem.tick();
    while(processed<serialCommands.size()) {
      auto cmd=serialCommands[processed++];
      if(cmd=="AT+CPIN?")feed("+CPIN: READY\r\nOK\r\n");
      else if(cmd=="AT+CEREG?")feed("+CEREG: 0,1\r\nOK\r\n");
      else if(cmd.find("AT+MNTP=")==0) {
        if(goodTime)feed("OK\r\n+MNTP: 0,\"26/11/11,08:00:00+32\"\r\n");
        else feed("+CCLK: \"26/11/11,08:00:00+32\"\r\nOK\r\n");
      }
      else if(cmd.find("AT+MQTTCONN=")==0)feed("OK\r\n+MQTTURC: \"conn\",0,0\r\n");
      else if(cmd=="AT+MQTTDISC=0")feed("OK\r\n+MQTTURC: \"conn\",0,2\r\n");
      else if(cmd.find("AT+MQTTSUB=")==0)feed("+MQTTSUB: 0,77\r\nOK\r\n+MQTTURC: \"suback\",0,77,1\r\n");
      else if(cmd.find("AT+MQTTPUB=")==0) {
        feed(">");modem.tick();
        feed("+MQTTPUB: 0,42,100\r\nOK\r\n");
        if(wantAck)feed("+MQTTURC: \"puback\",0,42,0\r\n");
      }
      else if(cmd=="AT+MPOF=0")feed("OK\r\nPOWER DOWN\r\n");
      else if(cmd.find("AT+MSSLCERTWR=")==0) {feed(">");modem.tick();feed("OK\r\n");}
      else if(cmd=="AT+MSSLCFG=\"sni\",0,1")feed("ERROR\r\n"); // old firmware optional capability
      else if(cmd=="ATI")feed("ML307C TEST FIRMWARE\r\nOK\r\n");
      else if(cmd=="AT" && !goodTime)feed("+MNTP: 0,\"26/11/11,08:00:00+32\"\r\nOK\r\n");
      else feed("OK\r\n");
    }
    if(fragmented && !serialRx.empty()) {auto bytes=serialRx;serialRx.clear();for(char c:bytes){serialRx.push_back(c);modem.tick();}}
    else modem.tick();
    if(modem.ready())return true;
    fakeMillis+=100;
  }
  return false;
}
int testDrivers() {
  // Failure diagnostics identify the command stage without retaining commands/credentials.
  resetFake();bt::Config diagnosticConfig;Modem diagnostic;diagnostic.begin();CHECK(diagnostic.start(diagnosticConfig));
  feed("OK\r\n");diagnostic.tick();feed("+CME ERROR: 50\r\n");diagnostic.tick();
  CHECK(diagnostic.failureStep==1 && diagnostic.errorCode==50);
  CHECK(diagnostic.status.find("1")!=std::string::npos);
  feed("OK\r\n");diagnostic.tick();feed("OK\r\nPOWER OFF\r\n");diagnostic.tick();
  CHECK(diagnostic.shutdownConfirmed && !diagnostic.busy());
  resetFake();bt::Config c;c.deviceId="master-test";c.broker="localhost";c.mqttUser="test";c.tls=false;c.labPlaintext=true;c.workSeconds=180;
  Modem modem;modem.begin();CHECK(fakePins[5]==LOW);CHECK(modem.start(c));CHECK(!modem.start(c));
  CHECK(advance(modem,true,true));CHECK(modem.ready());CHECK(modem.registered);CHECK(modem.connected);CHECK(modem.clockValid);
  CHECK(fakeSetTimeCalls==1 && fakeSetTimeEpoch==1794355200ULL);
  CHECK(std::any_of(serialCommands.begin(),serialCommands.end(),[](const std::string& s){return s.find("AT+CCLK=\"")==0 && s.substr(s.size()-4)=="+00\"";}));
  CHECK(std::find(serialCommands.begin(),serialCommands.end(),"AT+MQTTCFG=\"reconn\",0,0,20,0")!=serialCommands.end());
  CHECK(modem.publish("test/raw","{\"ok\":true}"));CHECK(!modem.publish("test/raw","second"));
  // Only AT OK and a mismatched PUBACK: upload must remain unconfirmed.
  feed(">\r\n+MQTTPUB: 0,42,100\r\nOK\r\n+MQTTURC: \"puback\",0,41,0\r\n");modem.tick();CHECK(!modem.published);
  CHECK(serialPayloads.back()=="{\"ok\":true}");
  feed("+MQTTURC: \"puback\",0,42,0\r\n");modem.tick();CHECK(modem.published);
  CHECK(modem.publish("test/cmd/run/completed","",true));
  CHECK(serialCommands.back().substr(serialCommands.back().size()-5)==",0,\"\"");
  feed("+MQTTPUB: 0,43,31\r\nOK\r\n+MQTTURC: \"puback\",0,43,0\r\n");modem.tick();
  CHECK(modem.published && modem.ready() && modem.connected);CHECK(!modem.shutdownAttempted);
  modem.shutdown();feed("OK\r\n");modem.tick();feed("OK\r\nPOWER DOWN\r\n");modem.tick();CHECK(!modem.busy());CHECK(modem.shutdownConfirmed);CHECK(fakePins[5]==LOW);
  // Missing AT response gives one finite power pulse then bounded shutdown.
  resetFake();Modem dead;dead.begin();CHECK(dead.start(c));fakeMillis+=1600;dead.tick();CHECK(fakePins[5]==LOW);for(int i=0;i<11;++i){fakeMillis+=1601;dead.tick();}CHECK(fakePins[5]==HIGH);
  fakeMillis+=3001;dead.tick();CHECK(fakePins[5]==LOW);fakeMillis+=6001;dead.tick();fakeMillis+=30001;dead.tick();
  fakeMillis+=4000;dead.tick();fakeMillis+=16000;dead.tick();CHECK(dead.busy());CHECK(dead.shutdownAttempts==2);
  fakeMillis+=16000;dead.tick();CHECK(!dead.busy());CHECK(!dead.published);CHECK(!dead.shutdownConfirmed && !dead.shutdownError.empty());
  auto countOff=[](){return std::count(serialCommands.begin(),serialCommands.end(),"AT+MPOF=0");};
  CHECK(countOff()==2);fakeMillis+=100000;dead.tick();CHECK(countOff()==2);
  CHECK(dead.start(c));silentProbes(dead);CHECK(fakePins[5]==LOW);CHECK(dead.status.find("电源状态未确认")!=std::string::npos);
  resetFake();Modem cancelled;cancelled.begin();CHECK(cancelled.start(c));silentProbes(cancelled);CHECK(fakePins[5]==HIGH);cancelled.shutdown();CHECK(fakePins[5]==LOW);
  // ERROR proves the modem is alive; it must not generate an automatic key pulse.
  resetFake();Modem liveError;liveError.begin();CHECK(liveError.start(c));feed("ERROR\r\n");liveError.tick();CHECK(fakePins[5]==LOW);CHECK(liveError.status.find("不按电源键")!=std::string::npos);
  // A prior successful transaction followed by an unconfirmed shutdown is also not power-off.
  resetFake();Modem knownOn;knownOn.begin();CHECK(knownOn.start(c));CHECK(advance(knownOn));knownOn.shutdown();
  fakeMillis+=4000;knownOn.tick();fakeMillis+=16000;knownOn.tick();fakeMillis+=16000;knownOn.tick();CHECK(!knownOn.busy() && !knownOn.shutdownConfirmed);
  CHECK(knownOn.start(c));silentProbes(knownOn);CHECK(fakePins[5]==LOW);
  // Confirmed power-off permits another boot in the same ESP run, including after a pulse.
  resetFake();Modem twice;twice.begin();CHECK(twice.start(c));silentProbes(twice);CHECK(fakePins[5]==HIGH);
  twice.shutdown();feed("OK\r\n");twice.tick();feed("POWER OFF\r\n");twice.tick();CHECK(twice.shutdownConfirmed && !twice.busy());
  CHECK(twice.start(c));silentProbes(twice);CHECK(fakePins[5]==HIGH);
  // SIM failure is retried with a delay, capped at five queries and the job budget.
  resetFake();Modem simRetry;reachSim(simRetry,c);CHECK(serialCommands.back()=="AT+CPIN?");feed("+CME ERROR: 13\r\n");simRetry.tick();size_t commandsBefore=serialCommands.size();
  fakeMillis+=999;simRetry.tick();CHECK(serialCommands.size()==commandsBefore);++fakeMillis;simRetry.tick();CHECK(serialCommands.back()=="AT+CPIN?");
  feed("+CPIN: READY\r\nOK\r\n");simRetry.tick();CHECK(serialCommands.back()=="AT+CEREG?");CHECK(simRetry.simStatus=="SIM 已就绪" && fakePins[5]==LOW);
  resetFake();Modem badSim;reachSim(badSim,c);
  for(int i=0;i<5;++i){feed("+CME ERROR: 13\r\n");badSim.tick();fakeMillis+=1001;badSim.tick();}
  CHECK(std::count(serialCommands.begin(),serialCommands.end(),"AT+CPIN?")==5);CHECK(badSim.failureStep==3 && badSim.status.find("停止本轮")!=std::string::npos);
  resetFake();Modem pin;reachSim(pin,c);feed("+CPIN: SIM PIN\r\nOK\r\n");pin.tick();CHECK(pin.failureStep==3 && pin.status.find("PIN/PUK")!=std::string::npos);CHECK(std::count(serialCommands.begin(),serialCommands.end(),"AT+CPIN?")==1);
  resetFake();Modem notReady;reachSim(notReady,c);feed("+CPIN: NOT READY\r\nOK\r\n");notReady.tick();CHECK(serialCommands.back()=="AT+CPIN?");CHECK(notReady.simStatus!="SIM 已就绪");
  fakeMillis+=c.workSeconds*1000UL+1;notReady.tick();CHECK(notReady.status.find("工作窗口超时")!=std::string::npos);
  // A connection without usable time/CA must never silently downgrade TLS.
  resetFake();c.tls=true;c.caPem="-----BEGIN CERTIFICATE-----\ntest\n-----END CERTIFICATE-----\n";
  Modem tls;tls.begin();CHECK(tls.start(c));CHECK(advance(tls));
  CHECK(std::find(serialCommands.begin(),serialCommands.end(),"AT+MSSLCFG=\"auth\",0,1")!=serialCommands.end());
  CHECK(std::find(serialCommands.begin(),serialCommands.end(),"AT+MSSLCFG=\"ignoreverify\",0,0")!=serialCommands.end());
  CHECK(std::find(serialCommands.begin(),serialCommands.end(),"AT+MSSLCFG=\"ignorestamp\",0,0")!=serialCommands.end());
  CHECK(serialPayloads.front()==c.caPem);
  CHECK(!tls.publish("test/raw",std::string(1025,'A')));
  tls.shutdown();feed("OK\r\n");tls.tick();fakeMillis+=16000;tls.tick();CHECK(tls.busy());fakeMillis+=16000;tls.tick();CHECK(!tls.busy());CHECK(!tls.shutdownConfirmed);
  CHECK(tls.shutdownAttempted && tls.shutdownAttempts==2 && !tls.shutdownError.empty());
  // Retry succeeds: never send a third power-off command, and release GPIO5.
  resetFake();Modem recovered;recovered.begin();CHECK(recovered.start(c));CHECK(advance(recovered));
  serialCommands.clear();recovered.shutdown();feed("OK\r\n");recovered.tick();feed("ERROR\r\n");recovered.tick();
  CHECK(recovered.busy() && recovered.shutdownAttempts==2);feed("POWER DOWN\r\n");recovered.tick();
  CHECK(!recovered.busy() && recovered.shutdownConfirmed && recovered.shutdownError.empty());CHECK(countOff()==2 && fakePins[5]==LOW);
  // Delayed MNTP outside its transaction, or CCLK during it, must not set time.
  resetFake();Modem stale;stale.begin();CHECK(stale.start(c));CHECK(!advance(stale,true,false,false));
  CHECK(!stale.clockValid && fakeSetTimeCalls==0);
  CHECK(std::none_of(serialCommands.begin(),serialCommands.end(),[](const std::string& s){return s.find("AT+MQTTCONN=")==0;}));
  feed("+MNTP: 0,\"26/11/11,08:00:00+32\"\r\n");stale.tick();CHECK(!stale.clockValid && fakeSetTimeCalls==0);
  resetFake();fakeSetTimeResult=-1;Modem badClock;badClock.begin();CHECK(badClock.start(c));CHECK(!advance(badClock));CHECK(!badClock.clockValid);
  // Daily time-only job must obtain time and shut down without connecting MQTT.
  resetFake();Modem timeOnly;timeOnly.begin();CHECK(timeOnly.start(c,true));advance(timeOnly);
  CHECK(timeOnly.clockValid);CHECK(timeOnly.networkPassed);CHECK(!timeOnly.busy());CHECK(timeOnly.shutdownConfirmed);
  CHECK(std::none_of(serialCommands.begin(),serialCommands.end(),[](const std::string& s){return s.find("AT+MQTTCONN=")==0 || s.find("AT+MQTTPUB=")==0;}));
  // Daily synchronization is bounded and must not kill a healthy MQTT session on an NTP error.
  resetFake();Modem daily;daily.begin();CHECK(daily.start(c,false,true));CHECK(advance(daily));CHECK(daily.subscribed);
  int previousClockCalls=fakeSetTimeCalls;CHECK(daily.syncClock());
  feed("+MNTP: 0,\"26/11/11,00:00:00+00\"\r\n");daily.tick();CHECK(fakeSetTimeCalls==previousClockCalls);
  feed("OK\r\n");daily.tick();feed("ERROR\r\n");daily.tick();daily.tick();CHECK(daily.ready() && daily.connected && !daily.clockValid);
  CHECK(daily.syncClock());feed("ERROR\r\n");daily.tick();daily.tick();CHECK(daily.ready() && daily.connected);
  CHECK(daily.syncClock());feed("OK\r\n");daily.tick();feed("OK\r\n+MNTP: 0,\"26/11/11,00:00:00+00\"\r\n");daily.tick();
  CHECK(daily.ready() && daily.connected && daily.clockValid && fakeSetTimeEpoch==1794355200ULL);
  // NVS double-slot, failed writes and corrupted new slot keep last good record.
  Persistence p;CHECK(p.begin());std::string out;
  CHECK(p.inspect("new-record",out)==Persistence::ReadResult::Missing);
  fakeNvs["damaged0"]="bad-json";CHECK(p.inspect("damaged",out)==Persistence::ReadResult::Corrupt);
  CHECK(!p.read("config",out));CHECK(p.write("config","first"));CHECK(p.read("config",out));CHECK(out=="first");
  fakeNvsFailWrite=true;CHECK(!p.write("config","second"));fakeNvsFailWrite=false;CHECK(p.read("config",out));CHECK(out=="first");
  fakeNvsCorruptWrite=true;CHECK(!p.write("config","second"));fakeNvsCorruptWrite=false;CHECK(p.read("config",out));CHECK(out=="first");
  CHECK(p.write("config","second"));CHECK(p.read("config",out));CHECK(out=="second");
  // Corrupt latest committed slot; loader falls back to previous valid record.
  for(auto& pair:fakeNvs)if(pair.second.find("second")!=std::string::npos)pair.second="corrupt";
  CHECK(p.read("config",out));CHECK(out=="first");
  JsonDocument legacyRecord;legacyRecord["revision"]=1;legacyRecord["crc"]=bt::checksum("legacy-string");legacyRecord["payload"]="legacy-string";
  String legacyText;serializeJson(legacyRecord,legacyText);Preferences oldPrefs;CHECK(oldPrefs.putString("legacy0",legacyText)>0);
  CHECK(p.read("legacy",out));CHECK(out=="legacy-string");CHECK(p.write("legacy","new-blob"));CHECK(p.read("legacy",out));CHECK(out=="new-blob");
  std::string large(5000,'A');CHECK(p.write("config",large));CHECK(p.read("config",out));CHECK(out==large);
  fakeNvsCorruptWrite=true;CHECK(!p.write("config",std::string(6000,'B')));fakeNvsCorruptWrite=false;
  CHECK(p.read("config",out));CHECK(out==large);
  fakeNvsFailWrite=true;CHECK(!p.write("config","smaller"));fakeNvsFailWrite=false;
  CHECK(p.read("config",out));CHECK(out==large);
  CHECK(p.write("config","smaller"));CHECK(p.read("config",out));CHECK(out=="smaller");
  CHECK(!p.write("config",std::string(9501,'X')));CHECK(p.read("config",out));CHECK(out=="smaller");
  // The measured master has a larger NVS partition. Copy project records into
  // a separate namespace, commit readiness last, and preserve the old store.
  fakeNvs.clear();fakeNvsTypes.clear();fakeExtraNvs.clear();fakeExtraNvsTypes.clear();
  Persistence small;CHECK(small.begin());CHECK(small.write("config","old-config"));CHECK(small.write("runtime","old-runtime"));CHECK(small.write("snapshot","old-cache"));CHECK(small.write("remote","old-receipts"));
  fakeNvs["foreign-key"]="untouched";auto original=fakeNvs;
  fakeExtraPartitionAvailable=true;Persistence expanded;CHECK(expanded.begin());CHECK(std::string(expanded.backend())=="smsdata" && expanded.migrated());CHECK(fakeNvs==original);
  CHECK(expanded.read("config",out) && out=="old-config");CHECK(expanded.read("snapshot",out) && out=="old-cache");CHECK(expanded.read("runtime",out) && out=="old-runtime");CHECK(expanded.read("remote",out) && out=="old-receipts");
  CHECK(expanded.freeEntries()==2048);
  CHECK(expanded.write("config","new-config"));CHECK(small.write("config","stale-legacy"));
  Persistence rebooted;CHECK(rebooted.begin());CHECK(rebooted.read("config",out) && out=="new-config");CHECK(!rebooted.migrated());
  fakeExtraNvs.erase("config0");fakeExtraNvs.erase("config1");CHECK(rebooted.inspect("config",out)==Persistence::ReadResult::Corrupt);
  // An interrupted migration never activates partial records; retry from the intact source.
  fakeNvs=original;fakeExtraNvs.clear();fakeExtraNvsTypes.clear();fakeNvsWriteBudget=2;
  Persistence interrupted;CHECK(!interrupted.begin());CHECK(fakeExtraNvs["_btcstate"]=="migrating");CHECK(fakeNvs==original);
  fakeNvsWriteBudget=-1;Persistence resumed;CHECK(resumed.begin());CHECK(resumed.read("config",out) && out=="old-config");CHECK(resumed.read("remote",out) && out=="old-receipts");CHECK(fakeNvs==original);
  // Explicit factory reset is resumable and cannot resurrect the old migrated config.
  fakeExtraNvs["_btcstate"]="reset";Persistence resetting;CHECK(resetting.begin());CHECK(resetting.inspect("config",out)==Persistence::ReadResult::Missing);CHECK(fakeNvs["foreign-key"]=="untouched");
  CHECK(!small.read("config",out));CHECK(resetting.write("config","factory-default"));Persistence resetReboot;CHECK(resetReboot.begin());CHECK(resetReboot.read("config",out) && out=="factory-default");
  fakeNvsPartitionInitResult=12;auto targetBefore=fakeExtraNvs;Persistence initError;CHECK(!initError.begin());CHECK(fakeExtraNvs==targetBefore);
  fakeNvsPartitionInitResult=0;fakeExtraNvs["_btcstate"]="unknown";targetBefore=fakeExtraNvs;Persistence badMarker;CHECK(!badMarker.begin());CHECK(fakeExtraNvs==targetBefore);
  fakeExtraPartitionAvailable=false;fakeNvsWriteBudget=-1;
  std::cout<<"PASS "<<checks<<" driver/power/serial-fragmentation/NVS checks (hardware mocked)\n";return checks;
}
