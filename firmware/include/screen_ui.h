#pragma once
#include <U8g2lib.h>
#include <string>
#include <stdint.h>
struct ScreenView {
  bool master=false,wifi=false,cell=false,mqtt=false,ble=false,locked=false;
  int battery=-1,page=0;
  uint64_t epoch=0;
  std::string title,lines[3],rolling,id;
};
void renderScreen(U8G2& s,const ScreenView& v);
