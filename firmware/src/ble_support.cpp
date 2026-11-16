// Raw AD helpers retained from reference storage.cpp. Product persistence uses
// separate, checked JSON records; no built-in real capture is linked.
#include "storage.h"
#include <cstring>
bool validAdData(const uint8_t* d,size_t n) {
  if(n>31)return false;
  for(size_t i=0;i<n;) {if(!d[i] || i+d[i]+1>n)return false; i+=d[i]+1;} return true;
}
bool findAdValue(const uint8_t* d,size_t n,uint8_t type,uint8_t* out,uint8_t& count) {
  if(!validAdData(d,n))return false;
  for(size_t i=0;i<n;i+=d[i]+1) if(d[i+1]==type) {count=d[i]-1;memcpy(out,d+i+2,count);return true;} return false;
}
String hexString(const uint8_t* d,size_t n) {const char* h="0123456789ABCDEF";String s;for(size_t i=0;i<n;++i){s+=h[d[i]>>4];s+=h[d[i]&15];}return s;}
