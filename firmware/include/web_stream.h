#pragma once
#include <algorithm>
#include <cstddef>
namespace bt {
// Both plain HTML and binary gzip data stay in memory-mapped ESP32 flash.
template<class Sink>
bool streamWebBytes(const char* bytes,size_t length,Sink sink,size_t chunk=1024) {
  if(!chunk)return false;
  for(size_t offset=0;offset<length;) {
    size_t n=std::min(chunk,length-offset);
    if(!sink(bytes+offset,n))return false;
    offset+=n;
  }
  return true;
}
}
