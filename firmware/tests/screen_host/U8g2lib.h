#pragma once
// Host adapter uses the REAL U8g2 C renderer and fonts, with no display bus.
#include "u8g2.h"
#include <cstring>
#include <string>
#include <time.h>
#ifdef _WIN32
inline tm* localtime_r(const time_t* value,tm* out) {return localtime_s(out,value)==0?out:nullptr;}
#endif
class U8G2 {
 public:
  u8g2_t u{};
  uint8_t pixels[1024]{};
  u8x8_display_info_t info{};
  int cursorX=0,cursorY=0;
  U8G2() {
    info.tile_width=16;info.tile_height=8;info.pixel_width=128;info.pixel_height=64;
    u8g2_GetU8x8(&u)->display_info=&info;
    u8g2_SetupBuffer(&u,pixels,8,u8g2_ll_hvline_vertical_top_lsb,&u8g2_cb_r0);
  }
  void clearBuffer() {memset(pixels,0,sizeof(pixels));}
  void setFont(const uint8_t* f) {u8g2_SetFont(&u,f);}
  void setCursor(int x,int y) {cursorX=x;cursorY=y;}
  void print(const char* s) {cursorX+=u8g2_DrawUTF8(&u,cursorX,cursorY,s);}
  void drawLine(int x,int y,int a,int b) {u8g2_DrawLine(&u,x,y,a,b);}
  void drawHLine(int x,int y,int w) {u8g2_DrawHLine(&u,x,y,w);}
  void drawVLine(int x,int y,int h) {u8g2_DrawVLine(&u,x,y,h);}
  void drawBox(int x,int y,int w,int h) {u8g2_DrawBox(&u,x,y,w,h);}
  void drawFrame(int x,int y,int w,int h) {u8g2_DrawFrame(&u,x,y,w,h);}
  void drawRFrame(int x,int y,int w,int h,int r) {u8g2_DrawRFrame(&u,x,y,w,h,r);}
  void drawCircle(int x,int y,int r) {u8g2_DrawCircle(&u,x,y,r,U8G2_DRAW_ALL);}
  void drawPixel(int x,int y) {u8g2_DrawPixel(&u,x,y);}
  void setClipWindow(int x,int y,int a,int b) {u8g2_SetClipWindow(&u,x,y,a,b);}
  void setMaxClipWindow() {u8g2_SetMaxClipWindow(&u);}
  void sendBuffer() {}
};
