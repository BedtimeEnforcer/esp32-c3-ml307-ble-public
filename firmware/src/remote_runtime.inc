// Included once inside main.cpp's device-owner namespace. No work runs in an
// MQTT callback; callbacks only copy bounded packets into the owner's queue.
uint64_t scheduledWake() {
  uint64_t now=epochNow();if(!now)return 0;
  auto w=bt::workWindow(cfg,now,remote.completed);
  uint64_t next=w.next;
  if(remote.slot && now<remote.slotEnd) {
    next=remote.attempts>=cfg.retryAttempts?remote.slotEnd:std::max(now+1,remote.retryAt);
    if(netRecovery.waiting && !netRecovery.allowed(millis()))next=std::max<uint64_t>(next,now+netRecovery.remaining(millis()));
    next=std::min(next,remote.slotEnd);
  } else if(w.active())next=now+1;
  uint64_t sync=lastSyncEpoch && now>=lastSyncEpoch?lastSyncEpoch+bt::DaySeconds:now+cfg.retryBaseSeconds;
  if(sync<=now)sync=now+std::max<uint32_t>(60,timeRecovery.remaining(millis()));
  return next?std::min(next,sync):sync;
}
void remoteConfigJson(JsonDocument& d,int page) {
  d["v"]=1;d["revision"]=cfg.revision;d["device_id"]=cfg.deviceId;d["page"]=page;
  auto c=d["values"].to<JsonObject>();
  if(page==0) {
    c["sleep"]=IsMaster && cfg.sleep;
    auto a=c["slots"].to<JsonArray>();for(auto s:cfg.slots){auto v=a.add<JsonObject>();v["start"]=s.start;v["end"]=s.end;}
#define FIELD(k) c[#k]=cfg.k
    FIELD(scanEvery);FIELD(scanSeconds);FIELD(uploadEvery);FIELD(workSeconds);FIELD(screenSeconds);FIELD(staleSeconds);FIELD(maintenanceBootSeconds);
    FIELD(retryAttempts);FIELD(retryBaseSeconds);FIELD(retryMaxSeconds);FIELD(connectSeconds);FIELD(recoveryRestart);FIELD(restartBatches);
  } else {
    FIELD(targetMac);FIELD(generation);FIELD(fixed);FIELD(simulate);FIELD(strict);FIELD(rollOffset);FIELD(rollLength);FIELD(rollMirror);
    FIELD(batteryMode);FIELD(usableMah);FIELD(sleepMa);FIELD(activeMa);FIELD(modemMa);
    c["fixedAdv"]=bt::hex(cfg.fixedFrame.adv);c["fixedRsp"]=bt::hex(cfg.fixedFrame.rsp);c["fixedPdu"]=cfg.fixedFrame.pdu;
#undef FIELD
  }
}
void queueRemote(bool plannedSleep,bool full) {
  JsonDocument d;d["v"]=1;d["device_id"]=cfg.deviceId;d["role"]=IsMaster?"master":"slave";d["reported_at"]=epochNow();
  d["revision"]=cfg.revision;d["uptime"]=millis()/1000;d["time_valid"]=epochNow()!=0;
  d["power_saving"]=IsMaster && cfg.sleep;d["planned_sleep"]=IsMaster && plannedSleep;
  d["next_wake_at"]=IsMaster && cfg.sleep?scheduledWake():0;
  auto w=bt::workWindow(cfg,epochNow(),remote.completed);d["next_window_at"]=w.active()?w.start:w.next;
  d["slot_start"]=remote.slot;d["slot_end"]=remote.slotEnd;d["attempts"]=remote.attempts;
  d["mqtt"]=IsMaster?modem.connected:subscriber.subscribed;d["wifi"]=WiFi.status()==WL_CONNECTED;
  if(WiFi.status()==WL_CONNECTED)d["wifi_rssi"]=WiFi.RSSI();else d["wifi_rssi"]=nullptr;
  if(IsMaster && modem.rssi>-127)d["cell_rssi"]=modem.rssi;else d["cell_rssi"]=nullptr;
  d["cell_registered"]=IsMaster && modem.registered;d["shutdown_fault"]=shutdownFault;
  if(IsMaster && cfg.batteryMode)d["battery_est_pct"]=battery.percent(cfg.usableMah);else d["battery_est_pct"]=nullptr;
  d["battery_estimated"]=true;d["ble_active"]=IsMaster?scanner.active():advertiser.active();
  d["mode"]=IsMaster?"capture":cfg.fixed?"fixed":"follow";
  bool matches=false;const bt::Frame* frame=!IsMaster && cfg.fixed?&cfg.fixedFrame:hasSaved?&saved.frame:nullptr;
  d["rolling"]=frame?bt::rolling(*frame,cfg.rollOffset,cfg.rollLength,cfg.rollMirror,matches):"";
  d["rolling_match"]=matches;d["changed_at"]=hasSaved && !(cfg.fixed && !IsMaster)?saved.changed:0;d["observed_at"]=hasSaved && !(cfg.fixed && !IsMaster)?saved.observed:0;
  d["received_at"]=lastReceivedEpoch;if(hasSaved)d["ble_rssi"]=saved.rssi;else d["ble_rssi"]=nullptr;
  if(!remote.history.empty()) {auto& h=remote.history.back();d["result_at"]=h.at;d["result_ok"]=h.success;d["result_reason"]=h.reason;}
  queuePacket(bt::deviceTopic(cfg,"state"),jsonString(d));
  if(!full)return;
  d.clear();d["v"]=1;d["device_id"]=cfg.deviceId;d["reported_at"]=epochNow();d["firmware"]="0.3.7-dev";
  d["storage_ok"]=storageOk;d["config_recovery"]=configRecovery;d["config_status"]=configStatus;
  if(IsMaster) {d["sim"]=modem.simStatus;d["modem_status"]=bt::shortText(modem.status);d["step"]=modem.failureStep;d["error"]=modem.errorCode;}
  queuePacket(bt::deviceTopic(cfg,"diagnostics"),jsonString(d));
  if(IsMaster && !hasSaved) {
    d.clear();d["v"]=1;d["device_id"]=cfg.masterId;d["heartbeat"]=true;d["target_observed"]=false;d["reported_at"]=epochNow();
    queuePacket(bt::topic(cfg,"status"),jsonString(d));
  }
  for(int page=0;page<2;++page) {d.clear();remoteConfigJson(d,page);queuePacket(bt::deviceTopic(cfg,"config/"+std::to_string(page)),jsonString(d));}
  for(int page=0;page<4;++page) {
    d.clear();d["v"]=1;d["reported_at"]=epochNow();auto a=d["items"].to<JsonArray>();
    for(size_t i=page*3;i<remote.history.size() && i<size_t(page+1)*3;++i) {
      auto& h=remote.history[i];auto v=a.add<JsonObject>();v["at"]=h.at;v["slot"]=h.slot;v["ok"]=h.success;v["reason"]=h.reason;
    }
    queuePacket(bt::deviceTopic(cfg,"history/"+std::to_string(page)),jsonString(d));
  }
  for(int page=0;page<8;++page) {
    d.clear();d["v"]=1;d["reported_at"]=epochNow();auto a=d["items"].to<JsonArray>();
    for(size_t i=page*2;i<remote.receipts.size() && i<size_t(page+1)*2;++i) {
      auto& r=remote.receipts[i];auto v=a.add<JsonObject>();v["id"]=r.command.id;v["action"]=r.command.action;v["state"]=r.state;v["at"]=r.at;v["expires_at"]=r.command.expires;v["reason"]=r.reason;
    }
    queuePacket(bt::deviceTopic(cfg,"ack/"+std::to_string(page)),jsonString(d));
  }
  // Receipts are published before tombstones. A closed browser does not keep
  // historical restart commands permanently retained at the broker.
  for(auto& r:remote.receipts)if(r.state!="received" && r.state!="running")queuePacket(bt::deviceTopic(cfg,"cmd/"+r.command.action+"/"+r.command.id),"");
}
void receiveCommand(const std::string& t,const std::string& payload) {
  if(payload.empty() || !storageOk)return;
  bt::Command c;std::string error;
  if(!bt::decodeCommand(cfg,t,payload,epochNow(),IsMaster,c,error)) {log("拒绝远程命令："+error);return;}
  if(auto* old=remote.find(c.id)) {
    // A changed payload must never replace the original command with this ID.
    if(old->command.action!=c.action || old->command.issued!=c.issued || old->command.expires!=c.expires) {log("命令ID被重复使用");return;}
    queueRemote();return;
  }
  if(!remote.reserve(c,epochNow(),error)) {
    log(error);JsonDocument d;auto a=d["items"].to<JsonArray>();auto r=a.add<JsonObject>();r["id"]=c.id;r["action"]=c.action;r["state"]="failed";r["reason"]=error;r["at"]=epochNow();
    queuePacket(bt::deviceTopic(cfg,"ack/rejected"),jsonString(d));queuePacket(t,"");return;
  }
  if(!saveRemote()) {remote.receipts.pop_back();return;}
  queueRemote();
}
void executeCommands() {
  if(!storageOk || !epochNow() || restartPending)return;
  for(auto& r:remote.receipts) {
    if(r.state!="received")continue;
    std::string id=r.command.id,action=r.command.action;
    if(epochNow()>=r.command.expires) {commandResult(id,"expired","命令已过期");return;}
    if(action=="run" && (cycleRunning || pendingUpload || rawQueued || !netRecovery.allowed(millis()))) {commandResult(id,"failed","设备忙或处于有限恢复等待");return;}
    r.state="running";r.at=epochNow();if(!saveRemote())return;
    if(action=="config") {
      if(r.command.revision!=cfg.revision || cfg.revision==UINT32_MAX) {commandResult(id,"failed","配置版本冲突，请刷新后重试");return;}
      JsonDocument patch;std::string error;bt::Config next=cfg;
      if(deserializeJson(patch,r.command.patch) || !bt::remotePatch(next,patch.as<JsonObjectConst>(),IsMaster,error)) {commandResult(id,"failed",error.empty()?"配置JSON非法":error);return;}
      next.revision=cfg.revision+1;next.appliedCommand=id;JsonDocument d;bt::configJson(next,d,true);
      if(!store.write("config",jsonString(d))) {commandResult(id,"failed","配置写入失败，旧配置保留");return;}
      bool newSource=next.targetMac!=cfg.targetMac || next.generation!=cfg.generation;
      bool oldSleep=cfg.sleep;cfg=next;
      if(newSource) {hasSaved=dirtyCache=false;captured=false;pendingUpload=false;scanner.stop();cycleRunning=false;rawQueued=false;}
      if(oldSleep && !cfg.sleep && remote.slot)retireSlot("已切换为持续在线模式",false);
      if(!IsMaster) {simulationEnabled=cfg.simulate;startAdvertising();}
      commandResult(id,"done","配置已保存并生效");return;
    }
    if(action=="restart") {
      r.reason="重启已安排，启动后确认完成";saveRemote();queueRemote();restartForMaintenance=true;restartPending=true;restartAt=millis()+3000;return;
    }
    if(action=="battery-full") {
      battery.usedMah=0;battery.calibrated=true;
      commandResult(id,saveRuntime()?"done":"failed","满充估算重置（以持久化结果为准）");return;
    }
    if(action=="run") {
      if(!startScan(true)) {commandResult(id,"failed","扫描未能启动");return;}
      runningCommand=id;manualStarted=millis();queueRemote();return;
    }
  }
}
bool sleepAdvertised=false,onlineSyncPending=false;
void remoteTick() {
  bool transport=IsMaster?modem.connected && modem.subscribed:subscriber.subscribed;
  if(remoteOnline && !transport) {remoteOnline=false;txActive=false;outgoing.clear();}
  if(transport && !remoteOnline && !closingNetwork) {
    remoteOnline=true;remoteConnectedAt=lastTelemetry=millis();sleepAdvertised=false;
    outgoing.clear();txActive=false;
    queuePacket(bt::deviceTopic(cfg,"presence"),"online");queueRemote();
  }
  if(IsMaster) {
    std::string t,p;int limit=8;while(limit-- && modem.pop(t,p))receiveCommand(t,p);
  }
  executeCommands();
  if(!runningCommand.empty() && since(manualStarted)>=cfg.workSeconds*1000UL)attemptFailed("立即执行任务超时");
  if(!remoteOnline || closingNetwork)return;
  if(IsMaster && cfg.sleep && since(sessionStarted)>=cfg.workSeconds*1000UL) {
    if(cycleRunning || pendingUpload)attemptFailed("单次联网工作窗口到期");
    closingNetwork=true;modem.shutdown();return;
  }
  if(IsMaster && pendingUpload && !rawQueued && hasSaved && captured) {
    refreshObservedClock();
    if(saved.seq==UINT64_MAX) {attemptFailed("序号已耗尽");return;}
    ++saved.seq;saved.reported=epochNow();saved.timeValid=saved.reported!=0;
    if(!saveSnapshot(saved)) {--saved.seq;attemptFailed("快照写入失败，未上传");return;}
    dirtyCache=false;rawQueued=true;
    queuePacket(bt::topic(cfg,"raw"),bt::encodeMessage(saved,cfg.batteryMode?battery.percent(cfg.usableMah):-1),true);
  }
  if(txActive) {
    bool acknowledged=IsMaster?modem.published:subscriber.confirmed(txId);
    if(acknowledged) {
      bool raw=outgoing.front().raw;outgoing.pop_front();txActive=false;
      if(raw) {
        rawQueued=pendingUpload=cycleRunning=false;lastUploadEpoch=epochNow();nextUpload=millis()+cfg.uploadEvery*1000UL;
        uploadState="本次采集上传成功（PUBACK）";netRecovery.success();failures=0;
        if(remote.slot && epochNow()<remote.slotEnd && saved.observed>=remote.slot)retireSlot("本时段采集上传成功",true);
        else {remote.record(epochNow(),true,"采集上传成功");saveRemote();}
        if(!runningCommand.empty()) {commandResult(runningCommand,"done","扫描及上传均成功（PUBACK）");runningCommand.clear();}
        saveRuntime();queueRemote();
      }
    } else if(since(txStarted)>=20000) {
      if(IsMaster)modem.shutdown();else {subscriber.stop();recoveryFailed(netRecovery,"远程上报PUBACK超时");}
      return;
    }
  }
  if(!txActive && !outgoing.empty()) {
    auto& packet=outgoing.front();
    bool sent=false;
    if(IsMaster)sent=modem.publish(packet.topic,packet.payload);
    else {txId=subscriber.publish(packet.topic,packet.payload);sent=txId>=0;}
    if(sent) {txActive=true;txStarted=millis();}
  }
  if(since(lastTelemetry)>=60000 && !sleepAdvertised) {lastTelemetry=millis();queueRemote(false,false);}
  bool idle=IsMaster && cfg.sleep && !maintenanceActive() && !maintenanceExitPending && !apOn && epochNow() && !cycleRunning && !pendingUpload && !rawQueued && !restartPending;
  if(idle && since(remoteConnectedAt)>=3000 && !sleepAdvertised) {sleepAdvertised=true;queueRemote(true,false);queuePacket(bt::deviceTopic(cfg,"presence"),"sleep");}
  if(idle && sleepAdvertised && outgoing.empty() && !txActive) {closingNetwork=true;modem.shutdown();}
  if(!idle)sleepAdvertised=false;
  if(IsMaster && !cfg.sleep && !txActive && outgoing.empty() && timeRecovery.allowed(millis())) {
    if(onlineSyncPending && modem.ready()) {
      onlineSyncPending=false;if(!modem.clockValid)recoveryFailed(timeRecovery,"在线每日校时失败");
    } else if(!onlineSyncPending && bt::syncDue(epochNow(),lastSyncEpoch) && modem.syncClock()) {modemTimeRecorded=false;onlineSyncPending=true;}
  }
}
