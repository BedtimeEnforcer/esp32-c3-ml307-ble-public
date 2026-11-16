#include "subscriber.h"
#include "remote.h"
bool Subscriber::start(const bt::Config& c,bool control) {
  if(client_)return true;
  if(c.broker.empty() || c.mqttUser.empty() || (c.tls && c.caPem.empty()))return false;
  // A reconnect can deliver sixteen retained commands and one raw snapshot
  // before loop() runs. Leave bounded headroom rather than reconnecting forever.
  if(!queue_)queue_=xQueueCreate(24,sizeof(Packet));
  if(!acks_)acks_=xQueueCreate(16,sizeof(int));
  if(!queue_ || !acks_)return false;
  topic_=bt::topic(c,"raw");
  control_=control;raw_=!c.fixed;controlTopic_=bt::deviceTopic(c,"cmd/");
  willTopic_=bt::deviceTopic(c,"presence");willPayload_="{\"v\":1,\"online\":false}";
  failed=false;receiving_=false;next_=0;subscription_=-1;
  controlSubscription_=-1;rawGranted_=!raw_;controlGranted_=!control_;
  esp_mqtt_client_config_t cfg{};
  cfg.host=c.broker.c_str();cfg.port=c.port;cfg.client_id=c.deviceId.c_str();
  cfg.username=c.mqttUser.c_str();cfg.password=c.mqttPass.c_str();
  cfg.transport=c.tls?MQTT_TRANSPORT_OVER_SSL:MQTT_TRANSPORT_OVER_TCP;
  cfg.cert_pem=c.tls?c.caPem.c_str():nullptr;
  cfg.skip_cert_common_name_check=false;cfg.keepalive=60;cfg.buffer_size=1536;cfg.network_timeout_ms=5000;
  cfg.disable_auto_reconnect=true; // Recovery policy in loop owns every attempt.
  cfg.reconnect_timeout_ms=10000;
  if(control_) {cfg.lwt_topic=willTopic_.c_str();cfg.lwt_msg=willPayload_.c_str();cfg.lwt_qos=1;cfg.lwt_retain=1;}
  client_=esp_mqtt_client_init(&cfg);
  if(!client_)return false;
  if(esp_mqtt_client_register_event(client_,MQTT_EVENT_ANY,event,this)!=ESP_OK || esp_mqtt_client_start(client_)!=ESP_OK) {stop();return false;}
  return true;
}
void Subscriber::stop() {
  if(client_) {esp_mqtt_client_stop(client_);esp_mqtt_client_destroy(client_);client_=nullptr;}
  connected=subscribed=false;receiving_=false;next_=0;
  if(queue_)xQueueReset(queue_);
  if(acks_)xQueueReset(acks_);
}
bool Subscriber::pop(std::string& payload) {
  std::string topic;return popPacket(topic,payload);
}
bool Subscriber::popPacket(std::string& topic,std::string& payload) {
  Packet p; if(!queue_ || xQueueReceive(queue_,&p,0)!=pdTRUE)return false;
  topic=p.topic;payload.assign(p.data,p.length);return true;
}
int Subscriber::publish(const std::string& topic,const std::string& payload,bool retain) {
  if(!connected || !client_ || payload.size()>bt::MaxPayload)return -1;
  return esp_mqtt_client_enqueue(client_,topic.c_str(),payload.data(),payload.size(),1,retain,true);
}
bool Subscriber::confirmed(int id) {
  int got;bool found=false;while(acks_ && xQueueReceive(acks_,&got,0)==pdTRUE)if(got==id)found=true;return found;
}
void Subscriber::event(void* context,esp_event_base_t,int32_t id,void* data) {
  auto& self=*static_cast<Subscriber*>(context);auto e=static_cast<esp_mqtt_event_handle_t>(data);
  if(id==MQTT_EVENT_CONNECTED) {
    self.connected=true;self.subscribed=false;
    if(self.raw_)self.subscription_=esp_mqtt_client_subscribe(self.client_,self.topic_.c_str(),1);
    if(self.control_)self.controlSubscription_=esp_mqtt_client_subscribe(self.client_,(self.controlTopic_+"#").c_str(),1);
    if((self.raw_ && self.subscription_<0) || (self.control_ && self.controlSubscription_<0)) {self.failed=true;++self.errors;}
  }
  else if(id==MQTT_EVENT_SUBSCRIBED && ((self.subscription_>=0 && e->msg_id==self.subscription_) || (self.controlSubscription_>=0 && e->msg_id==self.controlSubscription_))) {
    // One requested topic at QoS1: broker may grant QoS0/1, or refuse with 0x80.
    bool granted=e->data && e->data_len==1 && static_cast<uint8_t>(e->data[0])<=1;
    if(e->msg_id==self.subscription_)self.rawGranted_=granted;
    if(e->msg_id==self.controlSubscription_)self.controlGranted_=granted;
    self.subscribed=self.rawGranted_ && self.controlGranted_ && !self.failed;
    if(!granted) {self.failed=true;++self.errors;}
  }
  else if(id==MQTT_EVENT_DISCONNECTED) {self.connected=self.subscribed=false;self.receiving_=false;self.failed=true;}
  else if(id==MQTT_EVENT_ERROR) {
    if(e && e->error_handle) {
      const auto& error=*e->error_handle;
      self.lastErrorType=error.error_type;self.tlsError=error.esp_tls_last_esp_err;
      self.tlsStackError=error.esp_tls_stack_err;self.tlsCertFlags=error.esp_tls_cert_verify_flags;
      self.socketError=error.esp_transport_sock_errno;self.brokerCode=error.connect_return_code;
    } else self.lastErrorType=-2;
    ++self.errors;self.failed=true;
  }
  else if(id==MQTT_EVENT_PUBLISHED) {int mid=e->msg_id;if(xQueueSend(self.acks_,&mid,0)!=pdTRUE)self.failed=true;}
  else if(id==MQTT_EVENT_DATA) {
    if(e->current_data_offset==0) {
      std::string t=e->topic && e->topic_len>0 && e->topic_len<=256?std::string(e->topic,e->topic_len):"";
      self.receiving_=((self.raw_ && t==self.topic_) || (self.control_ && t.find(self.controlTopic_)==0)) && e->total_data_len>0 && e->total_data_len<=static_cast<int>(bt::MaxPayload);
      if(self.receiving_) {memcpy(self.assembling_.topic,t.data(),t.size());self.assembling_.topic[t.size()]=0;}
      self.next_=0;self.assembling_.length=e->total_data_len;
    }
    if(!self.receiving_)return;
    if(e->current_data_offset!=static_cast<int>(self.next_) || e->data_len<0 || self.next_+e->data_len>self.assembling_.length) {self.receiving_=false;++self.errors;return;}
    memcpy(self.assembling_.data+self.next_,e->data,e->data_len);self.next_+=e->data_len;
    if(self.next_==self.assembling_.length) {
      self.assembling_.data[self.next_]=0;
      if(xQueueSend(self.queue_,&self.assembling_,0)!=pdTRUE) {++self.errors;self.failed=true;}
      self.receiving_=false;
    }
  }
}
