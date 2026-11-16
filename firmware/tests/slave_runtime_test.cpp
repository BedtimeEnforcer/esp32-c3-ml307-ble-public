#define BT_RUNTIME_TEST 1
#include "../src/main.cpp"
#include <iostream>
static int checks=0;
#define CHECK(x) do{++checks;if(!(x)){std::cerr<<"FAIL slave runtime "<<__LINE__<<": "<<#x<<"\n";return 1;}}while(0)
static void received(const std::string& t,const std::string& p) {
  esp_mqtt_event_t e;e.topic=const_cast<char*>(t.data());e.topic_len=t.size();e.data=const_cast<char*>(p.data());e.data_len=e.total_data_len=p.size();fakeMqttEmit(MQTT_EVENT_DATA,e);
}
static void connect(bool fixed) {
  subscriber.stop();cfg.fixed=fixed;fakeIncrementSubscribe=true;fakeSubscribeId=7;
  subscriber.start(cfg,true);esp_mqtt_event_t e;fakeMqttEmit(MQTT_EVENT_CONNECTED,e);
  char granted=1;e.data=&granted;e.data_len=1;e.msg_id=7;fakeMqttEmit(MQTT_EVENT_SUBSCRIBED,e);
  if(!fixed){e.msg_id=8;fakeMqttEmit(MQTT_EVENT_SUBSCRIBED,e);}
}
static std::string cmd(const std::string& id,const std::string& patch,int revision=1){return "{\"v\":1,\"id\":\""+id+"\",\"action\":\"config\",\"device_id\":\"slave-test\",\"issued_at\":"+std::to_string(fakeEpoch)+",\"expires_at\":"+std::to_string(fakeEpoch+3600)+",\"expected_revision\":"+std::to_string(revision)+",\"patch\":"+patch+"}";}
int main() {
  cfg.deviceId="slave-test";cfg.prefix="pair-test";cfg.targetMac="C0:11:22:33:44:55";cfg.broker="mqtt.example.com";cfg.mqttUser="test";cfg.tls=false;cfg.labPlaintext=true;cfg.adminPass=cfg.apPass="test-password";
  storageOk=clockValid=bleOk=true;store.begin();displayAccess.configure(true);connect(false);CHECK(subscriber.subscribed);CHECK(!cfg.sleep);
  bt::Message m;m.master=cfg.masterId;m.seq=1;m.timeValid=true;m.reported=m.observed=m.changed=fakeEpoch;m.rssi=-66;m.frame.mac=cfg.targetMac;
  bt::unhex("0201061AFF4C00021500112233445566778899AABBCCDDEEFF11223344C5",m.frame.adv);bt::unhex("020A000303A5FD0E16A5FD6411223344C0001122334408094D426561636F6E",m.frame.rsp);
  received(bt::topic(cfg,"raw"),bt::encodeMessage(m,-1));slaveTick();CHECK(hasSaved && saved.seq==1 && advertiser.active());CHECK(remote.history.size()==1);CHECK(displayAccess.locked());
  received(bt::topic(cfg,"raw"),bt::encodeMessage(m,-1));slaveTick();CHECK(remote.history.size()==1);CHECK(saved.observed==m.observed);
  std::string id="disable-broadcast";received(bt::deviceTopic(cfg,"cmd/config/"+id),cmd(id,"{\"simulate\":false}"));slaveTick();executeCommands();CHECK(cfg.revision==2 && !advertiser.active());CHECK(remote.find(id)->state=="done");CHECK(displayAccess.locked());
  id="sleep-not-allowed";received(bt::deviceTopic(cfg,"cmd/config/"+id),cmd(id,"{\"sleep\":true}",2));slaveTick();executeCommands();CHECK(!cfg.sleep && cfg.revision==2);CHECK(remote.find(id)->state=="failed");maybeSleep();CHECK(!fakeSleeping);
  cfg.fixedFrame=m.frame;cfg.fixed=true;cfg.simulate=simulationEnabled=true;startAdvertising();connect(true);CHECK(subscriber.subscribed && advertiser.active());
  id="fixed-control";received(bt::deviceTopic(cfg,"cmd/config/"+id),cmd(id,"{\"scanSeconds\":25}",2));slaveTick();executeCommands();CHECK(cfg.scanSeconds==25 && cfg.revision==3 && cfg.fixed);CHECK(advertiser.active());
  queueRemote();for(auto& p:outgoing){CHECK(p.payload.size()<=1024);if(p.topic==bt::deviceTopic(cfg,"state")){JsonDocument d;CHECK(bt::jsonObject(p.payload,d));CHECK(d["changed_at"].as<uint64_t>()==0);CHECK(d["observed_at"].as<uint64_t>()==0);CHECK(d["power_saving"].as<bool>()==false);}}
  auto usb=[&](const std::string& op,const std::string& password,const std::string& patch="{}",uint32_t revision=0) {
    JsonDocument request,result;request["op"]=op;request["password"]=password;request["expectedRevision"]=revision;
    JsonDocument values;deserializeJson(values,patch);request["values"].set(values.as<JsonObjectConst>());
    usbRequest(jsonString(request),result);return jsonString(result);
  };
  CHECK(usb("diagnostic","bad").find("authentication_failed")!=std::string::npos);
  auto diagnostic=usb("diagnostic",cfg.adminPass);CHECK(diagnostic.find("\"ok\":true")!=std::string::npos);
  std::string diagnosticRequest="{\"op\":\"diagnostic\",\"password\":\""+cfg.adminPass+"\"}";
  auto frame=[&](const std::string& payload){return "BTLOCAL2 "+std::to_string(payload.size())+" "+std::to_string(bt::checksum(payload))+" "+payload;};
  JsonDocument framed;usbFrame(frame(diagnosticRequest),framed);CHECK(framed["ok"]==true && framed["usbProtocol"]==2);
  framed.clear();usbFrame("BTLOCAL2 999 1 "+diagnosticRequest,framed);CHECK(framed["error"]=="transport_integrity_failed");
  std::string damaged=frame(diagnosticRequest);damaged[damaged.size()-3]='x';framed.clear();usbFrame(damaged,framed);CHECK(framed["error"]=="transport_integrity_failed");
  framed.clear();usbFrame("BTLOCAL2 9501 1 {}",framed);CHECK(framed["error"]=="transport_integrity_failed");
  framed.clear();usbFrame("BTLOCAL {\"op\":\"config\"}",framed);CHECK(framed["error"]=="usb_v2_required" && !restartPending);
  framed.clear();usbFrame("BTLOCAL "+diagnosticRequest,framed);CHECK(framed["ok"]==true);
  CHECK(diagnostic.find("wifiPass")==std::string::npos && diagnostic.find("targetMac")==std::string::npos && diagnostic.find("11223344")==std::string::npos);
  CHECK(usb("maintenance",cfg.adminPass).find("display_locked")!=std::string::npos);CHECK(displayAccess.locked() && !apOn);
  displayAccess.configure(false);CHECK(usb("maintenance",cfg.adminPass).find("\"ok\":true")!=std::string::npos);CHECK(apOn && web.listening);
  web.close();CHECK(!web.listening);startAp();CHECK(web.listening);
  closeAp();CHECK(!apOn && !web.listening);CHECK(advertiser.active());displayAccess.configure(true);
  JsonDocument bad;usbRequest(std::string(9501,'x'),bad);CHECK(bad["error"]=="invalid_request");
  CHECK(usb("config",cfg.adminPass,"{}",cfg.revision+1).find("409")!=std::string::npos);CHECK(!restartPending);
  CHECK(usb("config",cfg.adminPass,"{\"wifiGeneration\":123}",cfg.revision).find("400")!=std::string::npos);
  CHECK(usb("config",cfg.adminPass,"{\"scanEvery\":1}",cfg.revision).find("400")!=std::string::npos);
  fakeNvsFailWrite=true;CHECK(usb("config",cfg.adminPass,"{}",cfg.revision).find("500")!=std::string::npos);CHECK(!restartPending);fakeNvsFailWrite=false;
  fakeCertificateParseError=-1;CHECK(usb("config",cfg.adminPass,"{\"caPem\":\"-----BEGIN CERTIFICATE-----invalid\"}",cfg.revision).find("400")!=std::string::npos);CHECK(!restartPending);fakeCertificateParseError=0;
  CHECK(usb("config",cfg.adminPass,"{\"wifiSsid\":\"new-network\",\"wifiPass\":\"local-test-secret\"}",cfg.revision).find("\"ok\":true")!=std::string::npos);
  CHECK(restartPending && displayAccess.locked() && advertiser.active());
  std::string persisted;CHECK(store.read("config",persisted));JsonDocument committed;CHECK(!deserializeJson(committed,persisted));
  CHECK(committed["revision"].as<uint32_t>()==cfg.revision+1);CHECK(committed["wifiGeneration"].as<uint32_t>()==cfg.wifiGeneration+1);
  CHECK(usb("config",cfg.adminPass,"{}",cfg.revision).find("409")!=std::string::npos);
  restartPending=false;usb("diagnostic","bad");usb("diagnostic","bad");usb("diagnostic","bad");
  CHECK(usb("diagnostic",cfg.adminPass).find("authentication_cooldown")!=std::string::npos);
  fakeMillis+=300001;CHECK(usb("diagnostic",cfg.adminPass).find("\"ok\":true")!=std::string::npos);
  CHECK(usb("backup","bad").find("authentication_failed")!=std::string::npos);
  CHECK(usb("backup",cfg.adminPass).find(cfg.adminPass)!=std::string::npos);
  storageOk=false;CHECK(usb("backup",cfg.adminPass).find("\"ok\":true")!=std::string::npos);storageOk=true;
  configRecovery=true;CHECK(usb("backup",cfg.adminPass).find("config_unavailable")!=std::string::npos);
  storageOk=false;slaveTick();CHECK(!subscriber.active() && !advertiser.active());configRecovery=false;storageOk=true;
  std::cout<<"PASS "<<checks<<" actual slave owner/follow/fixed/control/lock checks (hardware/network/time mocked)\n";
}
