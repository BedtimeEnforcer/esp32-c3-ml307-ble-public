#include "screen_ui.h"
#include <fstream>
#include <iostream>
#include <cassert>
void save(U8G2& s,const std::string& name) {
  std::ofstream f(name);f<<"P1\n128 64\n";
  for(int y=0;y<64;++y) {for(int x=0;x<128;++x)f<<((s.pixels[(y/8)*128+x]>>(y%8))&1)<<' ';f<<'\n';}
}
int main(int argc,char** argv) {
  if(argc!=2)return 1;
  ScreenView v;v.master=true;v.wifi=v.cell=v.mqtt=v.ble=true;v.battery=82;v.id="M001";
  v.epoch=1794394200;v.rolling="11223344";v.lines[2]="RAW / MIRROR OK";
  U8G2 s;
  const char* titles[]={"时钟","总览","当前滚动码","蓝牙采集","MQTT上传","数据记录","工作计划","设备维护"};
  for(int page=0;page<8;++page) {
    v.page=page;v.title=titles[page];
    if(page==3) {v.lines[0]="已完整抓取目标";v.lines[1]="C0:11:22:33:44:55";v.lines[2]="RSSI -61";}
    if(page==4) {v.lines[0]="上传成功 PUBACK";v.lines[1]="11-11 18:50:00";v.lines[2]="采集 11-11 18:49";}
    if(page==5) {v.lines[0]="主 master-01";v.lines[1]="11-11 18:49:50";v.lines[2]="SEQ 128";}
    if(page==6) {v.lines[0]="扫描 86400秒";v.lines[1]="上传 86400秒";v.lines[2]="校时 11-11 18:50";}
    if(page==7) {v.lines[0]="后台 192.168.4.1";v.lines[1]="恢复等待 0秒";v.lines[2]="息屏后重新锁定";}
    renderScreen(s,v);save(s,std::string(argv[1])+"/master-"+std::to_string(page)+".pbm");
  }
  v.page=5;v.locked=true;renderScreen(s,v);uint8_t locked[1024];memcpy(locked,s.pixels,1024);
  v.rolling="DEADBEEF";v.lines[0]="SECRET";v.id="SECRET";v.page=2;renderScreen(s,v);
  assert(memcmp(locked,s.pixels,1024)==0);save(s,std::string(argv[1])+"/locked.pbm");
  v.master=false;v.battery=-1;renderScreen(s,v);save(s,std::string(argv[1])+"/slave-clock.pbm");
  v.locked=false;v.page=2;v.title="当前滚动码";v.rolling="11223344AABBCCDD";renderScreen(s,v);save(s,std::string(argv[1])+"/long-rolling.pbm");
  std::cout<<"PASS actual U8g2 framebuffer rendering and locked-data invariance\n";
}
