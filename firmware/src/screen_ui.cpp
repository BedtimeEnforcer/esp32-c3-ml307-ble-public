#include "screen_ui.h"
#include <time.h>
namespace {
void text(U8G2& s,int x,int y,const std::string& t) {s.setCursor(x,y);s.print(t.c_str());}
void cross(U8G2& s,int x,int y) {s.drawLine(x,y,x+4,y+4);s.drawLine(x+4,y,x,y+4);}
void wifi(U8G2& s,int x,bool on,int y=0) {
  s.drawLine(x,y+3,x+5,y+1);s.drawLine(x+5,y+1,x+10,y+3);s.drawLine(x+2,y+5,x+5,y+4);s.drawLine(x+5,y+4,x+8,y+5);s.drawPixel(x+5,y+7);
  if(!on)cross(s,x+9,y+5);
}
void cell(U8G2& s,int x,bool on) {for(int i=0;i<4;++i)s.drawBox(x+i*3,8-i*2,2,i*2+1);if(!on)cross(s,x+10,5);}
void cloud(U8G2& s,int x,bool on,int y=0) {s.drawRFrame(x,y+3,13,6,2);s.drawFrame(x+4,y+1,5,4);if(!on)cross(s,x+10,y+5);else {s.drawVLine(x+6,y+4,5);s.drawLine(x+4,y+6,x+6,y+4);s.drawLine(x+8,y+6,x+6,y+4);}}
void bluetooth(U8G2& s,int x,bool on,int y=0) {s.drawVLine(x+4,y+1,9);s.drawLine(x+4,y+1,x+8,y+4);s.drawLine(x+8,y+4,x+1,y+9);s.drawLine(x+1,y+2,x+8,y+7);s.drawLine(x+8,y+7,x+4,y+10);if(!on)cross(s,x+10,y+5);}
void digit(U8G2& s,int x,int y,int n) {
  static const uint8_t masks[]={0x3f,6,0x5b,0x4f,0x66,0x6d,0x7d,7,0x7f,0x6f};
  uint8_t m=masks[n];
  if(m&1)s.drawBox(x+3,y,13,3);
  if(m&2)s.drawBox(x+16,y+3,3,10);
  if(m&4)s.drawBox(x+16,y+16,3,10);
  if(m&8)s.drawBox(x+3,y+26,13,3);
  if(m&16)s.drawBox(x,y+16,3,10);
  if(m&32)s.drawBox(x,y+3,3,10);
  if(m&64)s.drawBox(x+3,y+13,13,3);
}
}
void renderScreen(U8G2& s,const ScreenView& v) {
  s.clearBuffer();s.drawFrame(0,0,128,64);s.drawHLine(1,12,126);
  if(v.master) {cell(s,4,v.cell);wifi(s,23,v.wifi);cloud(s,43,v.mqtt);}
  else {wifi(s,4,v.wifi);cloud(s,24,v.mqtt);bluetooth(s,44,v.ble);}
  s.setFont(u8g2_font_5x7_tr);
  if(!v.locked)text(s,65,9,v.id.substr(0,4));
  if(v.battery>=0 && v.master) {
    s.drawFrame(92,2,31,8);s.drawBox(123,4,2,4);text(s,96,9,"~"+std::to_string(v.battery));
  } else text(s,97,9,"USB5V");
  int page=v.locked?0:v.page;
  s.setClipWindow(2,14,126,59);
  if(page==0) {
    if(v.epoch) {
      time_t epoch=v.epoch;tm t{};localtime_r(&epoch,&t);
      digit(s,6,17,t.tm_hour/10);digit(s,29,17,t.tm_hour%10);
      s.drawBox(53,25,3,3);s.drawBox(53,36,3,3);
      digit(s,61,17,t.tm_min/10);digit(s,84,17,t.tm_min%10);
      char sec[4];snprintf(sec,sizeof(sec),"%02d",t.tm_sec);text(s,110,44,sec);
      char date[24];strftime(date,sizeof(date),"%Y.%m.%d  %a",&t);text(s,8,58,date);
    } else {
      s.setFont(u8g2_font_logisoso24_tr);text(s,12,43,"--:--");
      s.setFont(u8g2_font_wqy12_t_gb2312);text(s,34,58,"等待校时");
    }
  } else if(page==1) {
    for(int i=0;i<6;++i) {
      int x=2+(i%3)*42,y=15+(i/3)*21;s.drawRFrame(x,y,40,19,2);
      if(i==0)wifi(s,x+3,v.wifi,y+3);
      else if(i==1)cloud(s,x+3,v.mqtt,y+3);
      else if(i==2)bluetooth(s,x+3,v.ble,y+3);
      else if(i==3) {s.drawCircle(x+8,y+8,5);s.drawLine(x+8,y+8,x+8,y+4);}
      else if(i==4) {s.drawFrame(x+4,y+3,9,11);s.drawHLine(x+6,y+6,5);}
      else {s.drawCircle(x+9,y+8,5);s.drawCircle(x+9,y+8,2);}
      s.setFont(u8g2_font_5x7_tr);
      const char* names[]={"NET","MQ","BLE","TIME","RAW","SET"};text(s,x+20,y+13,names[i]);
    }
  } else {
    s.setFont(u8g2_font_wqy12_t_gb2312);text(s,4,25,v.title);
    if(page==2) {
      s.setFont(u8g2_font_9x15B_tr);text(s,6,40,v.rolling.empty()?"--------":v.rolling.substr(0,8));
      if(v.rolling.size()>8)text(s,6,54,v.rolling.substr(8,8));
      else {s.setFont(u8g2_font_5x7_tr);text(s,6,56,v.lines[2]);}
    } else {
      s.setFont(u8g2_font_wqy12_t_gb2312);
      // Reserve 14 pixels per Chinese line; details continue on other pages.
      for(int i=0;i<2;++i)text(s,4,40+i*14,v.lines[i]);
    }
  }
  s.setMaxClipWindow();
  if(!v.locked && page)for(int i=0;i<8;++i) {if(i==page)s.drawBox(44+i*5,60,3,2);else s.drawPixel(45+i*5,61);}
  s.sendBuffer();
}
