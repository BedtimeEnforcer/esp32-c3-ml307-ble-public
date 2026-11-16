#pragma once
#include <stdint.h>
using esp_event_base_t=const char*;
using esp_mqtt_client_handle_t=void*;
struct esp_mqtt_error_codes_t {int error_type=0,esp_tls_last_esp_err=0,esp_tls_stack_err=0,esp_tls_cert_verify_flags=0,esp_transport_sock_errno=0,connect_return_code=0;};
struct esp_mqtt_event_t {
  esp_mqtt_error_codes_t* error_handle=nullptr;
  int msg_id=0,current_data_offset=0,topic_len=0,total_data_len=0,data_len=0;
  char* topic=nullptr;char* data=nullptr;
};
using esp_mqtt_event_handle_t=esp_mqtt_event_t*;
struct esp_mqtt_client_config_t {
  const char *lwt_topic=nullptr,*lwt_msg=nullptr;
  int lwt_qos=0,lwt_retain=0;
  const char *host,*client_id,*username,*password,*cert_pem;
  int port,transport,keepalive,buffer_size,network_timeout_ms,reconnect_timeout_ms;
  bool skip_cert_common_name_check,disable_auto_reconnect;
};
enum {MQTT_TRANSPORT_OVER_SSL,MQTT_TRANSPORT_OVER_TCP,MQTT_EVENT_ANY,MQTT_EVENT_CONNECTED,MQTT_EVENT_SUBSCRIBED,MQTT_EVENT_DISCONNECTED,MQTT_EVENT_ERROR,MQTT_EVENT_DATA,MQTT_EVENT_PUBLISHED};
constexpr int ESP_OK=0;
inline esp_mqtt_client_config_t fakeMqttConfig{};
inline int fakeSubscribeId=7;
inline bool fakeIncrementSubscribe=false;
inline int fakeMqttStops=0,fakeMqttDestroys=0;
inline bool fakeMqttInitFail=false;
inline void (*fakeMqttCallback)(void*,esp_event_base_t,int32_t,void*)=nullptr;
inline void* fakeMqttContext=nullptr;
inline esp_mqtt_client_handle_t esp_mqtt_client_init(const esp_mqtt_client_config_t* cfg) {
  fakeMqttConfig=*cfg;return fakeMqttInitFail?nullptr:reinterpret_cast<void*>(1);
}
inline int esp_mqtt_client_register_event(void*,int,void(*cb)(void*,esp_event_base_t,int32_t,void*),void* context) {fakeMqttCallback=cb;fakeMqttContext=context;return ESP_OK;}
inline int esp_mqtt_client_start(void*) {return ESP_OK;}
inline int esp_mqtt_client_stop(void*) {++fakeMqttStops;return ESP_OK;}
inline int esp_mqtt_client_destroy(void*) {++fakeMqttDestroys;return ESP_OK;}
inline int esp_mqtt_client_subscribe(void*,const char*,int) {return fakeIncrementSubscribe?fakeSubscribeId++:fakeSubscribeId;}
inline int esp_mqtt_client_enqueue(void*,const char*,const char*,int,int,int,bool) {return 13;}
inline void fakeMqttEmit(int id,esp_mqtt_event_t& event) {fakeMqttCallback(fakeMqttContext,nullptr,id,&event);}
