#pragma once
#include <string>
#include <deque>
#include <vector>
#include <algorithm>
#include <cstring>
#include <cctype>
#include <stdint.h>
#include <type_traits>
class String {
 public:
  std::string value;
  String()=default;
  String(const char* s):value(s?s:""){}
  String(const std::string& s):value(s){}
  String(char c):value(1,c){}
  template<typename T,typename=typename std::enable_if<std::is_integral<T>::value>::type> String(T n):value(std::to_string(n)){}
  size_t length()const{return value.size();}
  const char* c_str()const{return value.c_str();}
  bool isEmpty()const{return value.empty();}
  size_t write(uint8_t c){value+=static_cast<char>(c);return 1;}
  size_t write(const uint8_t* p,size_t n){value.append(reinterpret_cast<const char*>(p),n);return n;}
  operator std::string()const{return value;}
  bool startsWith(const char* p)const{return value.find(p)==0;}
  int indexOf(const char* p)const{auto n=value.find(p);return n==std::string::npos?-1:static_cast<int>(n);}
  void trim(){auto start=value.find_first_not_of(" \r\n\t");if(start==std::string::npos){value.clear();return;}value=value.substr(start,value.find_last_not_of(" \r\n\t")-start+1);}
  bool operator==(const char* s)const{return value==s;}
  bool operator!=(const char* s)const{return value!=s;}
  String& operator+=(const String& s){value+=s.value;return *this;}
  friend String operator+(const String& a,const String& b){return a.value+b.value;}
};
inline uint32_t fakeMillis=1;
inline int fakePins[32]{};
inline std::deque<char> serialRx;
inline std::vector<std::string> serialCommands, serialPayloads;
inline std::string pendingCommand;
inline void feed(const std::string& s){for(char c:s)serialRx.push_back(c);}
inline uint32_t millis(){return fakeMillis;}
constexpr int INPUT=0,OUTPUT=1,HIGH=1,LOW=0,SERIAL_8N1=0;
inline void pinMode(int,int){}
inline void digitalWrite(int pin,int state){fakePins[pin]=state;}
inline int digitalRead(int pin){return fakePins[pin];}
class HardwareSerial {
 public:
  explicit HardwareSerial(int){}
  void setRxBufferSize(int){}
  void begin(int,int,int,int){}
  void flush(){}
  void end(){}
  int available(){return serialRx.size();}
  int read(){char c=serialRx.front();serialRx.pop_front();return c;}
  void print(const String& s){pendingCommand+=s.value;if(pendingCommand.size()>=2 && pendingCommand.substr(pendingCommand.size()-2)=="\r\n"){serialCommands.push_back(pendingCommand.substr(0,pendingCommand.size()-2));pendingCommand.clear();}}
  void write(const uint8_t* d,size_t n){serialPayloads.emplace_back(reinterpret_cast<const char*>(d),n);}
};
