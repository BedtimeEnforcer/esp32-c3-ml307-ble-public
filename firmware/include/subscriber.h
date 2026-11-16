#pragma once
#include <Arduino.h>
#include <mqtt_client.h>
#include <freertos/queue.h>
#include "protocol.h"

class Subscriber {
 public:
  bool start(const bt::Config& config,bool control=false);
  void stop();
  bool pop(std::string& payload);
  bool popPacket(std::string& topic,std::string& payload);
  int publish(const std::string& topic,const std::string& payload,bool retain=true);
  bool confirmed(int id);
  bool active() const {return client_!=nullptr;}
  volatile bool failed=false;
  volatile bool connected=false, subscribed=false;
  volatile int errors=0;
  volatile int lastErrorType=-1,tlsError=0,tlsStackError=0,tlsCertFlags=0,socketError=0,brokerCode=0;
 private:
  struct Packet {size_t length; char topic[257];char data[bt::MaxPayload+1];};
  QueueHandle_t queue_=nullptr;
  QueueHandle_t acks_=nullptr;
  esp_mqtt_client_handle_t client_=nullptr;
  std::string topic_,controlTopic_,willTopic_,willPayload_;
  bool control_=false,raw_=true;
  Packet assembling_{};
  size_t next_=0;
  bool receiving_=false;
  int subscription_=-1,controlSubscription_=-1;
  bool rawGranted_=false,controlGranted_=false;
  static void event(void* self,esp_event_base_t base,int32_t id,void* data);
};
