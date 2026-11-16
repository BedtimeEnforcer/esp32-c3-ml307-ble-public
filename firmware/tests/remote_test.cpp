#include "remote.h"
#include <iostream>
static int checks=0;
#define CHECK(x) do{++checks;if(!(x)){std::cerr<<"FAIL remote "<<__LINE__<<": "<<#x<<"\n";return -1;}}while(0)
int testRemote() {
  bt::Config c;c.prefix="pair-test";c.deviceId="master-test";c.adminPass=c.apPass="test-password";std::string error;
  uint64_t start=1794355200ULL;
  CHECK(bt::slotsValid(c.slots,error));
  CHECK(!bt::workWindow(c,0).active());CHECK(bt::workWindow(c,start-1).next==start);
  auto w=bt::workWindow(c,start);CHECK(w.start==start && w.end==start+900);
  CHECK(bt::workWindow(c,start+899).active());CHECK(!bt::workWindow(c,start+900).active());
  auto done=bt::workWindow(c,start,{start});CHECK(!done.active() && done.next==start+44700);
  CHECK(bt::workWindow(c,start+44700).active());CHECK(bt::workWindow(c,start+45300).next==start+86400);
  c.slots={{1380,1440}};CHECK(bt::slotsValid(c.slots,error));CHECK(bt::workWindow(c,start+15*3600).active());
  c.slots={{1380,60}};CHECK(!bt::slotsValid(c.slots,error));c.slots={{480,480}};CHECK(!bt::slotsValid(c.slots,error));
  c.slots={{480,500},{490,510}};CHECK(!bt::slotsValid(c.slots,error));c.slots={{480,500},{500,510}};CHECK(bt::slotsValid(c.slots,error));
  c.slots={{1,2},{3,4},{5,6},{7,8},{9,10}};CHECK(bt::slotsValid(c.slots,error));c.slots.push_back({11,12});CHECK(!bt::slotsValid(c.slots,error));c.slots={};CHECK(bt::slotsValid(c.slots,error));
  JsonDocument patch;patch["slots"][0]["start"]=480;patch["slots"][0]["end"]=495;CHECK(bt::configPatch(c,patch.as<JsonObjectConst>(),true,error));CHECK(c.slots.size()==1);
  patch.clear();patch["sleep"]=true;CHECK(!bt::configPatch(c,patch.as<JsonObjectConst>(),false,error));CHECK(!c.sleep);
  patch.clear();patch["broker"]="evil.invalid";CHECK(!bt::remotePatch(c,patch.as<JsonObjectConst>(),true,error));patch.clear();patch["scanEvery"]=120;CHECK(bt::remotePatch(c,patch.as<JsonObjectConst>(),true,error));
  std::string payload="{\"v\":1,\"id\":\"test-command\",\"action\":\"config\",\"device_id\":\"master-test\",\"issued_at\":"+std::to_string(start)+",\"expires_at\":"+std::to_string(start+3600)+",\"expected_revision\":1,\"patch\":{\"scanEvery\":120}}";
  auto t=bt::deviceTopic(c,"cmd/config/test-command");bt::Command command;
  CHECK(bt::decodeCommand(c,t,payload,start,true,command,error));CHECK(command.revision==1 && command.id=="test-command");
  CHECK(!bt::decodeCommand(c,t+"-other",payload,start,true,command,error));CHECK(!bt::decodeCommand(c,t,payload,start-301,true,command,error));
  CHECK(!bt::decodeCommand(c,t,payload+"junk",start,true,command,error));CHECK(!bt::decodeCommand(c,t,std::string(1025,'x'),start,true,command,error));
  JsonDocument d;deserializeJson(d,payload);d["action"]="run";d.remove("patch");d.remove("expected_revision");std::string run;serializeJson(d,run);
  CHECK(!bt::decodeCommand(c,bt::deviceTopic(c,"cmd/run/test-command"),run,start,false,command,error));
  CHECK(bt::decodeCommand(c,bt::deviceTopic(c,"cmd/run/test-command"),run,start,true,command,error));
  bt::RemoteState state;CHECK(state.reserve(command,start,error));CHECK(!state.reserve(command,start,error) && error=="duplicate");
  state.find(command.id)->state="done";
  for(int i=1;i<16;++i) {command.id="command-"+std::to_string(i);CHECK(state.reserve(command,start,error));state.find(command.id)->state="done";}
  command.id="overflow";CHECK(!state.reserve(command,start,error));CHECK(state.receipts.size()==16);
  CHECK(state.reserve(command,start+3601,error));CHECK(state.receipts.size()==16);CHECK(state.find("overflow")->state=="expired");
  for(int i=0;i<20;++i)state.record(start+i,i%2,"synthetic history");
  CHECK(state.history.size()==12 && state.history.front().at==start+8);
  state.slot=start;state.slotEnd=start+900;state.attempts=2;state.slotReason="test failure";state.finishSlot(start+100,false,"failed");CHECK(!state.slot && state.completed.back()==start);
  std::string saved;CHECK(state.encode(saved));bt::RemoteState other;CHECK(other.decode(saved));CHECK(other.receipts.size()==16 && other.history.size()==12 && other.completed.size()==1);
  CHECK(other.history.back().reason=="failed");CHECK(!other.decode("{\"v\":99}"));
  std::string topic,data;CHECK(bt::parseModemMessage("+MQTTREAD: 0,7,\"btclone/test/cmd\",7,\"7B2278223A317D\"",topic,data));CHECK(data=="{\"x\":1}" && topic=="btclone/test/cmd");
  CHECK(bt::parseModemMessage("+MQTTREAD: 0,7,\"btclone/test/cmd\",0,\"\"",topic,data));CHECK(data.empty());
  CHECK(bt::parseModemMessage("+MQTTREAD: 0,7,\"btclone/test/cmd\",0",topic,data));CHECK(data.empty());
  CHECK(bt::parseModemMessage("+MQTTREAD: 0,7,\"btclone/test/cmd\",0,",topic,data));CHECK(data.empty());
  CHECK(!bt::parseModemMessage("+MQTTREAD: 0,7,\"btclone/test/cmd\",1",topic,data));
  for(const char* text:{"+MQTTREAD: 1,7,\"test\",7,\"7B2278223A317D\"","+MQTTREAD: 0,7,\"test\",6,\"7B2278223A317D\"","+MQTTREAD: 0,7,\"test\",1,\"GG\"","+MQTTREAD: 0,7,\"test\",1,\"41\",junk","+MQTTREAD: 0,7,\"test/#\",1,\"41\"","+MQTTREAD: 0,7,\"test\",1025,\"41\""})CHECK(!bt::parseModemMessage(text,topic,data));
  bt::RemoteState big;command.expires=start+3600;command.issued=start;command.patch=std::string(700,'x');bool rejected=false;
  for(int i=0;i<16;++i){command.id="big-"+std::to_string(i);if(!big.reserve(command,start,error)){rejected=true;break;}}
  CHECK(rejected);CHECK(big.encode(saved));CHECK(saved.size()<5501);CHECK(bt::shortText("一二三",4)=="一");
  JsonDocument strict;CHECK(bt::jsonObject("{\"ok\":true} \n",strict));CHECK(!bt::jsonObject("{\"ok\":true}x",strict));CHECK(!bt::jsonObject("{\"ok\":true}{}",strict));
  std::cout<<"PASS "<<checks<<" schedule/commands/dedup/history/ML307-downlink checks\n";return checks;
}
