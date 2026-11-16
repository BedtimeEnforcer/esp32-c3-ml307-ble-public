#include "subscriber.h"
#include "remote.h"
#include <iostream>
static int checks=0;
#define CHECK(x) do {++checks;if(!(x)){std::cerr<<"FAIL subscriber "<<__LINE__<<": "<<#x<<"\n";return -1;}} while(0)
int testSubscriber() {
  bt::Config c;c.deviceId="slave-test";c.broker="mqtt.example.com";c.mqttUser="test";c.prefix="pair-test";
  c.caPem="-----BEGIN CERTIFICATE-----\nTEST\n-----END CERTIFICATE-----";
  Subscriber subscriber;CHECK(subscriber.start(c));CHECK(subscriber.active());
  CHECK(fakeMqttConfig.disable_auto_reconnect);CHECK(!fakeMqttConfig.skip_cert_common_name_check);
  esp_mqtt_event_t event;fakeMqttEmit(MQTT_EVENT_CONNECTED,event);CHECK(subscriber.connected && !subscriber.subscribed);
  char refused=char(0x80);event.msg_id=7;event.data=&refused;event.data_len=1;
  fakeMqttEmit(MQTT_EVENT_SUBSCRIBED,event);CHECK(!subscriber.subscribed && subscriber.failed);CHECK(subscriber.errors==1);
  subscriber.stop();CHECK(!subscriber.active() && !subscriber.connected);CHECK(fakeMqttStops && fakeMqttDestroys);
  for(char granted:{char(0),char(1)}) {
    CHECK(subscriber.start(c));fakeMqttEmit(MQTT_EVENT_CONNECTED,event);
    event.msg_id=6;event.data=&granted;event.data_len=1;fakeMqttEmit(MQTT_EVENT_SUBSCRIBED,event);CHECK(!subscriber.subscribed);
    event.msg_id=7;fakeMqttEmit(MQTT_EVENT_SUBSCRIBED,event);CHECK(subscriber.subscribed && !subscriber.failed);subscriber.stop();
  }
  for(int invalid=0;invalid<4;++invalid) {
    CHECK(subscriber.start(c));fakeMqttEmit(MQTT_EVENT_CONNECTED,event);char bad=2;
    event.msg_id=7;event.data=invalid==0?nullptr:&bad;event.data_len=invalid==1?0:invalid==2?2:1;
    fakeMqttEmit(MQTT_EVENT_SUBSCRIBED,event);CHECK(!subscriber.subscribed && subscriber.failed);subscriber.stop();
  }
  CHECK(subscriber.start(c));fakeSubscribeId=-1;fakeMqttEmit(MQTT_EVENT_CONNECTED,event);CHECK(subscriber.failed && !subscriber.subscribed);subscriber.stop();fakeSubscribeId=7;
  CHECK(subscriber.start(c));fakeMqttEmit(MQTT_EVENT_CONNECTED,event);char granted=1;event.msg_id=7;event.data=&granted;event.data_len=1;fakeMqttEmit(MQTT_EVENT_SUBSCRIBED,event);
  std::string topic=bt::topic(c,"raw"),first="{\"x\":",last="1}";
  event.topic=const_cast<char*>(topic.data());event.topic_len=topic.size();event.current_data_offset=0;event.total_data_len=first.size()+last.size();event.data=const_cast<char*>(first.data());event.data_len=first.size();fakeMqttEmit(MQTT_EVENT_DATA,event);
  std::string payload;CHECK(!subscriber.pop(payload));
  event.topic=nullptr;event.topic_len=0;event.current_data_offset=first.size();event.data=const_cast<char*>(last.data());event.data_len=last.size();fakeMqttEmit(MQTT_EVENT_DATA,event);CHECK(subscriber.pop(payload));CHECK(payload==first+last);
  // Reject a fragment gap and discard a partial packet across stop/restart.
  event.current_data_offset=0;event.topic=const_cast<char*>(topic.data());event.topic_len=topic.size();event.data=const_cast<char*>(first.data());event.data_len=first.size();fakeMqttEmit(MQTT_EVENT_DATA,event);
  event.current_data_offset=first.size()+1;event.data=const_cast<char*>(last.data());event.data_len=last.size();fakeMqttEmit(MQTT_EVENT_DATA,event);CHECK(!subscriber.pop(payload));
  subscriber.stop();CHECK(subscriber.start(c));event.current_data_offset=first.size();fakeMqttEmit(MQTT_EVENT_DATA,event);CHECK(!subscriber.pop(payload));
  fakeMqttEmit(MQTT_EVENT_DISCONNECTED,event);CHECK(subscriber.failed && !subscriber.connected && !subscriber.subscribed);subscriber.stop();
  fakeMqttInitFail=true;CHECK(!subscriber.start(c));CHECK(!subscriber.active());fakeMqttInitFail=false;
  fakeSubscribeId=7;fakeIncrementSubscribe=true;CHECK(subscriber.start(c,true));fakeMqttEmit(MQTT_EVENT_CONNECTED,event);
  event.msg_id=7;event.data=&granted;event.data_len=1;fakeMqttEmit(MQTT_EVENT_SUBSCRIBED,event);CHECK(!subscriber.subscribed);
  event.msg_id=8;fakeMqttEmit(MQTT_EVENT_SUBSCRIBED,event);CHECK(subscriber.subscribed);
  CHECK(fakeMqttConfig.lwt_topic && fakeMqttConfig.lwt_msg && fakeMqttConfig.lwt_retain==1);
  CHECK(subscriber.publish(bt::deviceTopic(c,"state"),"{}") ==13);CHECK(!subscriber.confirmed(13));event.msg_id=13;fakeMqttEmit(MQTT_EVENT_PUBLISHED,event);CHECK(subscriber.confirmed(13));CHECK(!subscriber.confirmed(13));
  std::string ct=bt::deviceTopic(c,"cmd/restart/test"),cp="{}",receivedTopic;
  event.topic=const_cast<char*>(ct.data());event.topic_len=ct.size();event.total_data_len=cp.size();event.current_data_offset=0;event.data=const_cast<char*>(cp.data());event.data_len=cp.size();fakeMqttEmit(MQTT_EVENT_DATA,event);
  CHECK(subscriber.popPacket(receivedTopic,payload));CHECK(receivedTopic==ct && payload==cp);
  for(int i=0;i<17;++i)fakeMqttEmit(MQTT_EVENT_DATA,event);
  CHECK(!subscriber.failed);
  int drained=0;while(subscriber.popPacket(receivedTopic,payload))++drained;
  CHECK(drained==17); // full sixteen-command mailbox plus retained raw burst
  std::string wrong=bt::deviceTopic(c,"state");event.topic=const_cast<char*>(wrong.data());event.topic_len=wrong.size();fakeMqttEmit(MQTT_EVENT_DATA,event);CHECK(!subscriber.pop(payload));
  subscriber.stop();c.fixed=true;fakeSubscribeId=7;CHECK(subscriber.start(c,true));fakeMqttEmit(MQTT_EVENT_CONNECTED,event);event.msg_id=7;event.data=&granted;event.data_len=1;fakeMqttEmit(MQTT_EVENT_SUBSCRIBED,event);CHECK(subscriber.subscribed);
  esp_mqtt_error_codes_t transportError;transportError.error_type=1;transportError.esp_tls_last_esp_err=123;
  transportError.esp_tls_stack_err=-456;transportError.esp_tls_cert_verify_flags=8;transportError.esp_transport_sock_errno=111;transportError.connect_return_code=5;
  event.error_handle=&transportError;fakeMqttEmit(MQTT_EVENT_ERROR,event);
  CHECK(subscriber.failed && subscriber.lastErrorType==1 && subscriber.tlsError==123);
  CHECK(subscriber.tlsStackError==-456 && subscriber.tlsCertFlags==8 && subscriber.socketError==111 && subscriber.brokerCode==5);
  subscriber.stop();CHECK(subscriber.tlsError==123);
  event.error_handle=nullptr;fakeMqttEmit(MQTT_EVENT_ERROR,event);CHECK(subscriber.lastErrorType==-2);
  subscriber.stop();fakeSubscribeId=7;fakeIncrementSubscribe=false;
  std::cout<<"PASS "<<checks<<" actual subscriber callback/SUBACK/fragment/lifecycle checks (MQTT transport mocked)\n";return checks;
}
