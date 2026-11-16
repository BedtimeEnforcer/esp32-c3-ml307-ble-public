#pragma once
#include <deque>
#include <vector>
#include <cstring>
struct FakeQueue {size_t capacity,size;std::deque<std::vector<char>> packets;};
using QueueHandle_t=FakeQueue*;
constexpr int pdTRUE=1;
inline QueueHandle_t xQueueCreate(size_t capacity,size_t size) {return new FakeQueue{capacity,size,{}};}
inline int xQueueReset(QueueHandle_t q) {q->packets.clear();return pdTRUE;}
inline int xQueueReceive(QueueHandle_t q,void* out,int) {
  if(q->packets.empty())return 0;
  memcpy(out,q->packets.front().data(),q->size);q->packets.pop_front();return pdTRUE;
}
inline int xQueueSend(QueueHandle_t q,const void* packet,int) {
  if(q->packets.size()>=q->capacity)return 0;
  const char* data=static_cast<const char*>(packet);q->packets.emplace_back(data,data+q->size);return pdTRUE;
}
