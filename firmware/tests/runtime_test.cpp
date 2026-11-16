// Runs the real main.cpp scheduler + remote owner + real modem/subscriber code.
// GPIO, UART, NVS, BLE radio, sleep and wall clock are deterministic fakes.
#define BT_RUNTIME_TEST 1
#include "../src/main.cpp"
#include <iostream>
#include <iomanip>
static int checks=0;
#define CHECK(x) do{++checks;if(!(x)){std::cerr<<"FAIL runtime "<<__LINE__<<": "<<#x<<" (ms="<<fakeMillis<<", attempts="<<remote.attempts<<", failures="<<failures<<", sleep="<<fakeSleeping<<")\n";return 1;}}while(0)
static size_t handled=0;
static int mid=100;
static bool ackRaw=true;
static std::vector<std::pair<std::string,std::string>> publications;
static std::deque<std::pair<std::string,std::string>> commands;
static void respond() {
  while(handled<serialCommands.size()) {
    std::string cmd=serialCommands[handled++];
    if(cmd=="AT+CPIN?")feed("+CPIN: READY\r\nOK\r\n");
    else if(cmd=="AT+CEREG?")feed("+CEREG: 0,1\r\nOK\r\n");
    else if(cmd=="AT+CSQ")feed("+CSQ: 18,99\r\nOK\r\n");
    else if(cmd.find("AT+MNTP=")==0) {
      time_t t=fakeEpoch+28800;tm* u=gmtime(&t);char text[64];strftime(text,sizeof(text),"%y/%m/%d,%H:%M:%S+32",u);
      feed(std::string("OK\r\n+MNTP: 0,\"")+text+"\"\r\n");
    } else if(cmd.find("AT+MQTTCONN=")==0)feed("OK\r\n+MQTTURC: \"conn\",0,0\r\n");
    else if(cmd.find("AT+MQTTSUB=")==0)feed("+MQTTSUB: 0,77\r\nOK\r\n+MQTTURC: \"suback\",0,77,1\r\n");
    else if(cmd=="AT+MQTTREAD=0")feed("+MQTTREAD: 0,"+std::to_string(commands.size())+",100\r\nOK\r\n");
    else if(cmd=="AT+MQTTREAD=0,1") {
      auto p=commands.front();commands.pop_front();std::vector<uint8_t> bytes(p.second.begin(),p.second.end());
      feed("+MQTTREAD: 0,1,\""+p.first+"\","+std::to_string(bytes.size())+(bytes.empty()?"":",\""+bt::hex(bytes)+"\"")+"\r\nOK\r\n");
    } else if(cmd.find("AT+MQTTPUB=")==0) {
      size_t a=cmd.find('"'),b=cmd.find('"',a+1);std::string t=cmd.substr(a+1,b-a-1);
      std::string payload;
      const auto tail=cmd.substr(cmd.find_last_of(',')+1);
      if(tail=="0") {feed(">");continue;} // Omitted message at length zero waits for input.
      if(tail!="\"\"") {feed(">");modem.tick();payload=serialPayloads.back();}
      publications.push_back({t,payload});int id=++mid;
      const size_t remaining=t.size()+payload.size()+4;
      feed("+MQTTPUB: 0,"+std::to_string(id)+","+std::to_string(remaining+(remaining<128?2:3))+"\r\nOK\r\n");
      if(ackRaw || t!=bt::topic(cfg,"raw"))feed("+MQTTURC: \"puback\",0,"+std::to_string(id)+",0\r\n");
      if(t.find("/cmd/")!=std::string::npos && payload.empty()) {commands.push_back({t,""});feed("+MQTTURC: \"pubnmi\",0,1,0\r\n");}
    } else if(cmd=="AT+MPOF=0")feed("OK\r\nPOWER DOWN\r\n");
    else if(cmd=="AT+MQTTDISC=0")feed("OK\r\n+MQTTURC: \"conn\",0,2\r\n");
    else feed("OK\r\n");
  }
}
static void advanceMs(uint32_t ms,bool radio=true) {
  for(uint32_t n=0;n<ms && !fakeSleeping;n+=10) {
    masterTick();remoteTick();if(radio)respond();maybeSleep();
    fakeMillis+=10;if(fakeMillis%1000==1)++fakeEpoch;
  }
}
static void resetOwner(bool sleep=true) {
  maintenanceWindow.cancel();maintenanceExitPending=restartForMaintenance=false;maintenanceExitAt=0;buttonAt=0;web.close();nextBootMaintenance=0;
  fakeMillis=1;fakeEpoch=1794355200ULL;fakeSleeping=false;fakeSleepUs=0;
  fakeNvs.clear();fakeNvsFailWrite=fakeNvsCorruptWrite=false;
  serialRx.clear();serialCommands.clear();serialPayloads.clear();pendingCommand.clear();handled=0;publications.clear();commands.clear();ackRaw=true;
  cfg=bt::Config{};cfg.deviceId="master-test";cfg.prefix="pair-test";cfg.targetMac="C0:11:22:33:44:55";cfg.broker="mqtt.example.com";cfg.mqttUser="test";cfg.mqttPass="test-only";cfg.tls=false;cfg.labPlaintext=true;cfg.sleep=sleep;cfg.adminPass=cfg.apPass="test-password";
  cfg.slots={{480,495},{1225,1235}};cfg.scanSeconds=1;cfg.workSeconds=60;
  remote=bt::RemoteState{};saved=bt::Message{};battery=bt::Battery{};
  modem=Modem{};modem.begin();store.begin();scanner=BleScanner{};
  netRecovery=wifiRecovery=timeRecovery=bt::Recovery{};
  storageOk=bleOk=clockValid=true;hasSaved=dirtyCache=captured=changed=forceUpload=networkTest=publishPending=cycleRunning=restartPending=apOn=timeJob=pendingUpload=rawQueued=remoteOnline=txActive=closingNetwork=shutdownFault=sessionFailed=false;
  shutdownEpisodeHandled=true;modemTimeRecorded=false;configRecovery=false;configStatus="已加载有效配置";outgoing.clear();runningCommand.clear();
  sleepAdvertised=onlineSyncPending=false;lastTelemetry=lastSyncEpoch=0;nextScan=nextUpload=0;lastUploadEpoch=lastScanEpoch=0;failures=0;
  displayAccess.configure(true);
}
static ScanObservation target() {
  bt::Frame f;f.mac=cfg.targetMac;bt::unhex("0201061AFF4C00021500112233445566778899AABBCCDDEEFF11223344C5",f.adv);bt::unhex("020A000303A5FD0E16A5FD6411223344C0001122334408094D426561636F6E",f.rsp);
  ScanObservation o;o.snapshot=toSnapshot(f);return o;
}
static std::string envelope(const std::string& id,const std::string& action,const std::string& extra="") {
  return "{\"v\":1,\"id\":\""+id+"\",\"device_id\":\"master-test\",\"action\":\""+action+"\",\"issued_at\":"+std::to_string(fakeEpoch)+",\"expires_at\":"+std::to_string(fakeEpoch+3600)+extra+"}";
}
int main() {
  resetOwner();masterTick();CHECK(scanner.active() && remote.attempts==1 && remote.slot==fakeEpoch);CHECK(displayAccess.locked());
  scanner.input.push_back(target());advanceMs(10000);CHECK(!remote.history.empty() && remote.history.back().success);CHECK(remote.completed.size()==1);CHECK(fakeSleeping);CHECK(fakeSleepUs>12ULL*3600*1000000);
  CHECK(displayAccess.locked());CHECK(lastUploadEpoch && !pendingUpload);
  auto rawCount=std::count_if(publications.begin(),publications.end(),[](auto& p){return p.first==bt::topic(cfg,"raw");});CHECK(rawCount==1);
  bool planned=false;for(auto& p:publications)if(p.first==bt::deviceTopic(cfg,"state")){JsonDocument d;CHECK(!deserializeJson(d,p.second));if(d["planned_sleep"]==true)planned=true;CHECK(p.second.size()<=1024);}CHECK(planned);
  // Persist/reload completion, same slot must not execute twice after reset.
  std::string persisted;CHECK(remote.encode(persisted));bt::RemoteState restored;CHECK(restored.decode(persisted));CHECK(!bt::workWindow(cfg,1794355200ULL+30,restored.completed).active());
  // A cached old snapshot plus successful telemetry is not a successful scan.
  resetOwner();hasSaved=true;saved.frame.mac=cfg.targetMac;saved.observed=fakeEpoch-86400;advanceMs(4000);CHECK(remote.completed.empty());CHECK(remote.attempts==1);CHECK(remote.slotReason.find("MAC")!=std::string::npos);CHECK(!remote.history.size());
  // No radio responses: bounded work, persist reason, sleep no later than end.
  resetOwner();cfg.retryBaseSeconds=60;cfg.retryMaxSeconds=900;advanceMs(110000,false);CHECK(fakeSleeping);CHECK(!remote.completed.size());CHECK(remote.attempts==1);CHECK(failures==1);CHECK(remote.retryAt<=remote.slotEnd);
  // A PUBACK arriving after window end must not turn failure into success.
  resetOwner();cfg.slots={{480,481}};ackRaw=false;masterTick();scanner.input.push_back(target());advanceMs(2000);CHECK(rawQueued);
  fakeEpoch=1794355260ULL;masterTick();CHECK(remote.completed.size()==1);CHECK(!remote.history.back().success);CHECK(!pendingUpload);CHECK(!txActive);
  // Display remains locked while a valid remote configuration executes.
  resetOwner(false);std::string id="config-one",cmd=envelope(id,"config",",\"expected_revision\":1,\"patch\":{\"scanEvery\":120}");
  receiveCommand(bt::deviceTopic(cfg,"cmd/config/"+id),cmd);CHECK(remote.find(id) && remote.find(id)->state=="received");executeCommands();CHECK(cfg.scanEvery==120 && cfg.revision==2);CHECK(remote.find(id)->state=="done" && displayAccess.locked());
  receiveCommand(bt::deviceTopic(cfg,"cmd/config/"+id),cmd);executeCommands();CHECK(cfg.revision==2);
  id="conflict";receiveCommand(bt::deviceTopic(cfg,"cmd/config/"+id),envelope(id,"config",",\"expected_revision\":1,\"patch\":{\"scanEvery\":300}"));executeCommands();CHECK(cfg.scanEvery==120 && remote.find(id)->state=="failed");
  id="secret-change";receiveCommand(bt::deviceTopic(cfg,"cmd/config/"+id),envelope(id,"config",",\"expected_revision\":2,\"patch\":{\"mqttPass\":\"injected\"}"));executeCommands();CHECK(cfg.revision==2 && cfg.mqttPass=="test-only");CHECK(remote.find(id)->state=="failed");
  // NVS failure must prevent accepting/executing a new command.
  resetOwner(false);fakeNvsFailWrite=true;id="no-persistence";receiveCommand(bt::deviceTopic(cfg,"cmd/restart/"+id),envelope(id,"restart"));executeCommands();CHECK(!restartPending && !storageOk && !remote.find(id));
  // Continuous mode retains the real MQTT connection after successful upload.
  resetOwner(false);masterTick();scanner.input.push_back(target());advanceMs(10000);CHECK(modem.connected && modem.subscribed && !fakeSleeping);CHECK(!modem.shutdownAttempted);
  CHECK(lastUploadEpoch);CHECK(!cycleRunning);
  id="live-control";commands.push_back({bt::deviceTopic(cfg,"cmd/config/"+id),envelope(id,"config",",\"expected_revision\":1,\"patch\":{\"scanEvery\":180}")});feed("+MQTTURC: \"pubnmi\",0,1,100\r\n");advanceMs(3000);CHECK(cfg.scanEvery==180 && remote.find(id)->state=="done");CHECK(modem.connected);
  for(auto& p:publications)CHECK(p.second.size()<=1024);
  advanceMs(200000);CHECK(modem.connected && modem.subscribed && !modem.shutdownAttempted);CHECK(modem.rssi==-77);
  id="manual-run";commands.push_back({bt::deviceTopic(cfg,"cmd/run/"+id),envelope(id,"run")});feed("+MQTTURC: \"pubnmi\",0,1,100\r\n");advanceMs(500);CHECK(scanner.active());scanner.input.push_back(target());advanceMs(3000);CHECK(remote.find(id)->state=="done" && lastUploadEpoch);CHECK(displayAccess.locked());
  resetOwner(false);cfg.deviceId=std::string(32,'x');cfg.rollLength=8;hasSaved=true;saved.frame.adv.resize(31);saved.frame.rsp.resize(31);remote.record(fakeEpoch,false,std::string(96,'x'));outgoing.clear();queueRemote();
  CHECK(std::count_if(outgoing.begin(),outgoing.end(),[](const auto& p){return p.topic==bt::deviceTopic(cfg,"state");})==1);
  for(const auto& p:outgoing)CHECK(p.payload.size()<=1024);
  // Invalid stored config must remain intact, with background persistence stopped.
  resetOwner(false);fakeNvs["config0"]="corrupted";auto damaged=fakeNvs;
  CHECK(!loadConfiguration());CHECK(configRecovery && !storageOk);CHECK(fakeNvs==damaged);
  CHECK(!saveRuntime() && !saveRemote());CHECK(fakeNvs==damaged);
  resetOwner(false);CHECK(store.write("config","not-json"));damaged=fakeNvs;
  CHECK(!loadConfiguration() && configRecovery && fakeNvs==damaged);
  resetOwner(false);JsonDocument valid;bt::configJson(cfg,valid,true);CHECK(store.write("config",jsonString(valid)));
  CHECK(loadConfiguration() && !configRecovery && storageOk);
  JsonDocument backup,result;configBackup(backup);backup["role"]="slave";
  CHECK(restoreLocalConfig(backup,cfg.revision,result)==400);CHECK(!restartPending);
  backup["role"]="master";result.clear();CHECK(restoreLocalConfig(backup,cfg.revision+1,result)==409);CHECK(!restartPending);
  backup["config"]["revision"]=999;result.clear();CHECK(restoreLocalConfig(backup,cfg.revision,result)==200);CHECK(restartPending && restartForMaintenance);
  CHECK(store.read("config",persisted));JsonDocument restoredConfig;CHECK(!deserializeJson(restoredConfig,persisted));CHECK(restoredConfig["revision"].as<uint32_t>()==cfg.revision+1);
  // Storage fault must still pump the modem's bounded shutdown, not freeze it.
  resetOwner(false);advanceMs(5000);CHECK(modem.connected);storageOk=false;advanceMs(1000);CHECK(!modem.busy() && modem.shutdownConfirmed);CHECK(!remoteOnline);
  // Manual reset outside 08:30..09:00 leaves an offline, locked maintenance window.
  resetOwner();cfg.slots={{510,540}};lastSyncEpoch=fakeEpoch;beginMaintenanceWindow(bt::BootKind::PowerOn);
  CHECK(maintenanceActive() && maintenanceWindow.remaining(millis())==120);advanceMs(119000,false);
  CHECK(!fakeSleeping && !scanner.active() && !modem.busy() && displayAccess.locked());
  startAp();CHECK(!apOn);buttonAt=fakeMillis;fakeMillis+=1100;maybeSleep();CHECK(!fakeSleeping);
  maintenanceActivity();buttonAt=0;CHECK(maintenanceWindow.remaining(millis())==120);
  uint32_t gesture=fakeMillis;for(uint32_t duration:{100U,100U,1000U,100U,100U,1000U}) {gesture+=duration+100;displayAccess.release(duration,gesture);}fakeMillis=gesture;
  CHECK(!displayAccess.locked());startAp();CHECK(apOn && web.listening);
  fakeMillis+=300000;maybeSleep();CHECK(!fakeSleeping);requestMaintenanceExit();fakeMillis+=499;maintenanceTick();CHECK(apOn);
  ++fakeMillis;maintenanceTick();CHECK(!apOn && !web.listening && displayAccess.locked());maybeSleep();CHECK(fakeSleeping);
  // Scheduled and automatic/fault restarts keep the old power-saving behavior.
  for(auto kind:{bt::BootKind::Timer,bt::BootKind::AutomaticRestart,bt::BootKind::Fault}) {
    resetOwner();cfg.slots={{510,540}};lastSyncEpoch=fakeEpoch;beginMaintenanceWindow(kind);CHECK(!maintenanceActive());maybeSleep();CHECK(fakeSleeping);
  }
  // Maintenance does not stop an ordinary in-window capture/upload.
  resetOwner();beginMaintenanceWindow(bt::BootKind::ResetButton);masterTick();scanner.input.push_back(target());advanceMs(10000);
  CHECK(lastUploadEpoch && remote.completed.size()==1 && !fakeSleeping && maintenanceActive());CHECK(displayAccess.locked());
  maintenanceWindow.cancel();advanceMs(10000);CHECK(fakeSleeping);
  // No trusted time / failed network must not prevent access during manual grace.
  resetOwner();clockValid=false;netRecovery.defer(60,millis(),0);beginMaintenanceWindow(bt::BootKind::RequestedRestart);advanceMs(10000,false);
  CHECK(!fakeSleeping && maintenanceActive() && !clockValid);displayAccess.configure(false);startAp();CHECK(apOn);
  std::cout<<"PASS "<<checks<<" actual main/scheduler/remote-owner integration checks (hardware/network/time mocked)\n";
}
