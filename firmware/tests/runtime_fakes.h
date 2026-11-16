#pragma once
#include "Arduino.h"
#include "policy.h"
#include "remote.h"
#include "persistence.h"
#include "modem.h"
#include "subscriber.h"
#include "config.h"
#include <ctime>
#include <cmath>
inline tm* localtime_r(const time_t* value,tm* result){*result=*localtime(value);return result;}
inline void sntp_stop(){}
inline uint64_t fakeEpoch=1794355200ULL;
inline int fakeCertificateParseError=0;
struct mbedtls_x509_crt {};
inline void mbedtls_x509_crt_init(mbedtls_x509_crt*){}
inline int mbedtls_x509_crt_parse(mbedtls_x509_crt*,const unsigned char*,size_t){return fakeCertificateParseError;}
inline void mbedtls_x509_crt_free(mbedtls_x509_crt*){}
inline bool fakeSleeping=false;
inline uint64_t fakeSleepUs=0;
constexpr int WIFI_STA=1,WIFI_AP_STA=2,WIFI_OFF=0,WL_CONNECTED=3;
struct FakeIP {String toString() const{return "192.0.2.1";}};
struct FakeWiFi {
  int status()const{return WL_CONNECTED;}
  int RSSI()const{return -60;}
  FakeIP localIP()const{return {};}
  int softAPgetStationNum()const{return 1;}
  void setSleep(bool){}void softAPdisconnect(bool){}void mode(int){}void disconnect(bool){}
  bool softAP(const char*,const char*,int,bool,int){return true;}
} inline WiFi;
class WebServer {public:bool listening=false;explicit WebServer(int){}void begin(){listening=true;}void close(){listening=false;}};
constexpr int U8G2_R0=0,U8X8_PIN_NONE=255;
struct U8G2_SH1106_128X64_NONAME_F_HW_I2C {U8G2_SH1106_128X64_NONAME_F_HW_I2C(int,int,int=255,int=255){}void setPowerSave(int){}};
inline struct {void println(const char*){}void flush(){}} Serial;
constexpr int BLE_ADDR_RANDOM=1,BLE_HCI_ADV_RPT_EVTYPE_ADV_IND=0,BLE_HCI_ADV_RPT_EVTYPE_SCAN_IND=2,BLE_HCI_ADV_RPT_EVTYPE_NONCONN_IND=3;
struct Snapshot {char deviceMac[18]{};int addressType=1,advType=0;size_t advLength=0,responseLength=0,rawLength=0;uint8_t advData[31]{},responseData[31]{},rawData[62]{};};
struct ScanObservation {Snapshot snapshot;int rssi=-70;};
struct BleScanner {
  bool scanning=false;std::deque<ScanObservation> input;
  bool active()const{return scanning;}bool begin(){return true;}bool start(){scanning=true;return true;}void stop(){scanning=false;}
  void target(const char*){}bool pop(ScanObservation& value){if(input.empty())return false;value=input.front();input.pop_front();return true;}
  int seen()const{return 0;}std::string environment()const{return "[]";}
};
struct BleAdvertiser {
  bool advertising=false;bool active()const{return advertising;}void stop(){advertising=false;}
  bool start(const Snapshot&){advertising=true;return true;}std::string lastError()const{return "";}
};
constexpr int GPIO_NUM_5=5;
inline void gpio_hold_dis(int){}inline void gpio_hold_en(int){}inline void gpio_deep_sleep_hold_en(){}
inline void esp_sleep_enable_timer_wakeup(uint64_t us){fakeSleepUs=us;}
inline void esp_deep_sleep_start(){fakeSleeping=true;}
inline uint32_t esp_random(){return 12345;}
#define RTC_DATA_ATTR
#define RTC_NOINIT_ATTR
