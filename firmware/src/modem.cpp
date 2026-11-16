#include "modem.h"
#include "remote.h"
#include <sys/time.h>
#include <ctime>
static std::string readShape(const String& text) {
  // Only structural metadata: never keep MQTT topics, payloads or credentials.
  size_t fields=1,quotes=0,bytes=0;bool quoted=false;std::string sizes;
  auto at=std::string(text.c_str()).find(':');
  for(size_t i=at==std::string::npos?0:at+1;i<text.length();++i) {
    char c=text.c_str()[i];if(c=='"'){quoted=!quoted;++quotes;}
    if(c==',' && !quoted){sizes+=std::to_string(bytes)+",";bytes=0;++fields;}else ++bytes;
  }
  return "fields="+std::to_string(fields)+" quotes="+std::to_string(quotes)+" sizes="+sizes+std::to_string(bytes);
}

void Modem::begin() {pinMode(5,OUTPUT);digitalWrite(5,LOW);uart_.setRxBufferSize(8192);uart_.begin(115200,SERIAL_8N1,4,3);}
bool Modem::busy() const {return state_!=Idle && state_!=Done;}
void Modem::command(const String& text,uint32_t timeout,const std::string& data) {
  ok_=error_=false;response_="";payload_=data;prompt_=!data.empty();sentAt_=millis();timeout_=timeout;
  // Deliberately do not log commands: MQTT credentials may be present.
  uart_.print(text);uart_.print("\r\n");
}
bool Modem::start(const bt::Config& c,bool test,bool control) {
  if(busy())return false;
  cfg_=c;netOnly_=test;registered=connected=published=clockValid=shutdownConfirmed=false;
  sim_=clockSeen_=networkPassed=false;epoch=0;step_=0;started_=millis();status="检测 ML307";state_=Probe;
  shutdownAttempted=false;shutdownAttempts=0;shutdownError.clear();probeMisses_=simRetries_=0;
  probeResponded_=simBlocked_=false;simStatus="未检查";
  control_=control;subscribed=readPending_=false;subMid_=subAckMid_=subCode_=-1;inbox_.clear();lastRead_=lastSignal_=millis();rssi=-127;
  failureStep=errorCode=connectionCode=-1;networkClock.clear();receivedBytes=0;
  while(uart_.available())uart_.read();
  line_="";discard_=false;
  command("AT",1500);return true;
}
void Modem::receive() {
  size_t budget=4096;
  while(uart_.available() && budget--) {
    char c=uart_.read();++receivedBytes;
    if(c=='>' && prompt_ && !discard_) {uart_.write(reinterpret_cast<const uint8_t*>(payload_.data()),payload_.size());payload_.clear();prompt_=false;line_="";continue;}
    if(c=='\r')continue;
    if(c!='\n') {if(line_.length()<3072 && !discard_)line_+=c;else {discard_=true;line_="";}continue;}
    if(discard_) {discard_=false;error_=true;continue;}
    line_.trim(); if(line_.isEmpty())continue;
    bool poweredOff=line_=="POWER OFF" || line_=="POWER DOWN" || line_.indexOf("MSHUTDOWN")>=0;
    if(state_==Probe)probeResponded_=true;
    if(state_!=PowerOff && state_!=OffWait && state_!=Done && !poweredOff)powerSeenOn_=true;
    if(line_=="OK")ok_=true;
    if(line_=="ERROR" || line_.startsWith("+CME ERROR")) {
      error_=true;
      int code;if(sscanf(line_.c_str(),"+CME ERROR: %d",&code)==1)errorCode=code;
    }
    if(response_.length()+line_.length()<4096)response_+=line_+"\n";
    if(line_.startsWith("+CPIN:")) {
      std::string value=line_.c_str()+6;auto first=value.find_first_not_of(' ');value=first==std::string::npos?"":value.substr(first);
      sim_=value=="READY";
      simBlocked_=value=="SIM PIN" || value=="SIM PUK" || value=="SIM PIN2" || value=="SIM PUK2";
      simStatus=sim_?"SIM 已就绪":simBlocked_?"SIM 需要 PIN/PUK，请在手机中处理":"SIM 尚未就绪";
    }
    if(line_.startsWith("+CEREG:")) {int n=-1,s=-1;if(sscanf(line_.c_str(),"+CEREG: %d,%d",&n,&s)==2)registered=s==1||s==5;}
    if(((state_==Setup && step_==7) || state_==SyncClock) && millis()-sentAt_<=timeout_ && line_.startsWith("+MNTP: 0,")) {
      uint64_t t; if(bt::parseClock(line_.c_str(),t)) {epoch=t;clockSeen_=true;networkClock=line_.c_str();}
    }
    if(line_.startsWith("+CSQ:")) {int csq;if(sscanf(line_.c_str(),"+CSQ: %d",&csq)==1 && csq>=0 && csq<=31)rssi=-113+csq*2;}
    if(state_==Setup && control_ && step_==30 && line_.startsWith("+MQTTSUB:")) {int cid,mid;if(sscanf(line_.c_str(),"+MQTTSUB: %d,%d",&cid,&mid)==2 && cid==0 && mid>=0 && mid<=65535)subMid_=mid;}
    if(state_==ReadCheck && line_.startsWith("+MQTTREAD:")) {int cid,count,len;if(sscanf(line_.c_str(),"+MQTTREAD: %d,%d,%d",&cid,&count,&len)==3 && cid==0)readPending_=count>0;}
    if(state_==ReadMessage && line_.startsWith("+MQTTREAD:")) {
      std::string t,p;
      lastReadShape=readShape(line_);
      if(!bt::parseModemMessage(line_.c_str(),t,p) || inbox_.size()>=8) {error_=true;++readFailures;lastReadFailureShape=lastReadShape;}
      else if(!p.empty())inbox_.push_back({t,p});else lastEmptyReadShape=lastReadShape;
    }
    if(line_.startsWith("+MQTTURC:")) {
      int cid,code;
      if(sscanf(line_.c_str(),"+MQTTURC: \"conn\",%d,%d",&cid,&code)==2 && cid==0) {
        connected=code==0;
        if(state_==ClearSession)disconnectSeen_=code==2;
        else if(state_!=PowerOff && state_!=OffWait) {connectionCode=code;if(code>1)error_=true;}
      }
      if(line_.indexOf("\"timeout\",0,")>=0)error_=true;
      if(control_ && line_.indexOf("\"pubnmi\",0,")>=0)readPending_=true;
      if(control_ && state_==Setup && step_==30) {
        int cid,mid,code,n=0;
        if(sscanf(line_.c_str(),"+MQTTURC: \"suback\",%d,%d,%d%n",&cid,&mid,&code,&n)==3 && cid==0 && n==static_cast<int>(line_.length())) {subAckMid_=mid;subCode_=code;if(code>1)error_=true;}
      }
    }
    if(state_==Publish)ack_.line(line_.c_str());
    if(poweredOff) {
      powerSeenOn_=powerPulseDone_=false;
      if(state_==PowerOff || state_==OffWait)shutdownConfirmed=true;
      else {connected=registered=false;error_=true;}
    }
    line_="";
  }
}
void Modem::fail(const char* reason) {
  status=reason;
  if(state_==Setup || state_==SimWait) {failureStep=step_;status+="（步骤 "+std::to_string(step_)+"）";}
  shutdown();
}
void Modem::startClock(bool online) {
  // This ML307C revision returns UTC civil time with the stored RTC zone suffix.
  // Set the module's zone to UTC before MNTP instead of guessing an 8-hour correction.
  // Preserve the current UTC date when available; a cold boot still requires successful MNTP before TLS.
  clockOnly_=online;clockSeen_=clockValid=false;epoch=0;networkClock.clear();
  time_t seed=time(nullptr);if(seed<1704067200)seed=1704067200;
  tm utc=*gmtime(&seed);char text[32];strftime(text,sizeof(text),"%y/%m/%d,%H:%M:%S+00",&utc);
  state_=ClockZone;command(String("AT+CCLK=\"")+text+"\"",5000);status="设置模组 UTC 时区";
}
void Modem::next() {
  ++step_;
  switch(step_) {
    case 1:command("ATE0");break;
    case 2:command("ATI");break;
    case 3:command("AT+CPIN?");status="检查 SIM";break;
    case 4:command("AT+CEREG?");status="等待 4G 注网";break;
    case 5:
      if(cfg_.apn.empty()) {next();break;}
      command(String("AT+CGDCONT=1,\"IP\",\"")+cfg_.apn.c_str()+"\"");break;
    case 6:command("AT+CGACT=1,1",30000);status="激活数据网络";break;
    case 7:
      startClock(false);break;
    case 8:
      networkPassed=true;
      if(netOnly_) {status=clockValid?"4G 联网与校时成功（未测 MQTT）":"4G 联网成功，校时失败";shutdown();break;}
      if(cfg_.broker.empty() || cfg_.mqttUser.empty()) {fail("MQTT 参数未配置");break;}
      // An ESP-only reset can leave the separately powered modem's MQTT client alive.
      disconnectSeen_=false;state_=ClearSession;command("AT+MQTTDISC=0",3000);status="清理模组旧 MQTT 会话";break;
    case 9:command("AT+MQTTCFG=\"cid\",0,1");break;
    case 10:command("AT+MQTTCFG=\"clean\",0,1");break;
    case 11:command(control_?"AT+MQTTCFG=\"encoding\",0,0,1":"AT+MQTTCFG=\"encoding\",0,0,0");break;
    case 12:
      if(!cfg_.tls) {step_=21;next();break;}
      if(!clockSeen_ || cfg_.caPem.empty()) {fail("TLS 需要有效网络时间与 CA 证书");break;}
      command(String("AT+MSSLCERTWR=\"")+cfg_.modemCa.c_str()+"\",0,"+cfg_.caPem.size(),10000,cfg_.caPem);break;
    case 13:command("AT+MSSLCFG=\"auth\",0,1");break;
    case 14:command(String("AT+MSSLCFG=\"cert\",0,\"")+cfg_.modemCa.c_str()+"\",\"\",\"\"");break;
    case 15:command("AT+MSSLCFG=\"version\",0,3");break;
    case 16:command("AT+MSSLCFG=\"ignorestamp\",0,0");break;
    case 17:command("AT+MSSLCFG=\"ignoreverify\",0,0");break;
    case 18:command("AT+MSSLCFG=\"negotime\",0,30");break;
    case 19:command("AT+MSSLCFG=\"sni\",0,1");break; // optional; older ML307 may auto-SNI or lack it
    case 20:command("AT+MQTTCFG=\"ssl\",0,1,0");break;
    case 21:next();break;
    case 22:
      if(!cfg_.tls) {command("AT+MQTTCFG=\"ssl\",0,0");break;}
      next();break;
    case 23:command("AT+MQTTCFG=\"keepalive\",0,60");break;
    case 24:
      // Disable module-internal retries: the ESP recovery policy owns the budget.
      command("AT+MQTTCFG=\"reconn\",0,0,20,0");break;
    case 25:
      if(control_) {command("AT+MQTTCFG=\"cached\",0,1");break;}
      command(String("AT+MQTTCONN=0,\"")+cfg_.broker.c_str()+"\","+cfg_.port+",\""+cfg_.deviceId.c_str()+"\",\""+cfg_.mqttUser.c_str()+"\",\""+cfg_.mqttPass.c_str()+"\"",45000);
      status="连接 MQTT（TLS/SNI 待实测）";break;
    case 26:
      if(!control_) {state_=Ready;status="MQTT 已连接，等待发布";break;}
      command("AT+MQTTCFG=\"willoption\",0,1,1,1");break;
    case 27:command(String("AT+MQTTCFG=\"willpayload\",0,\"")+bt::deviceTopic(cfg_,"presence").c_str()+"\",\"offline\"");break;
    case 28:command("AT+CSQ");break;
    case 29:
      command(String("AT+MQTTCONN=0,\"")+cfg_.broker.c_str()+"\","+cfg_.port+",\""+cfg_.deviceId.c_str()+"\",\""+cfg_.mqttUser.c_str()+"\",\""+cfg_.mqttPass.c_str()+"\"",45000);
      status="连接 MQTT（TLS/SNI 待实测）";break;
    case 30:command(String("AT+MQTTSUB=0,\"")+bt::deviceTopic(cfg_,"cmd/#").c_str()+"\",1",15000);break;
    default:state_=Ready;status="MQTT 已连接，等待发布";break;
  }
}
void Modem::tick() {
  receive();uint32_t now=millis();
  if((state_==Setup || state_==SimWait || state_==ClearSession || (state_==ClockZone && !clockOnly_) || state_==Probe || state_==BootWait || state_==Pulse || (!control_ && (state_==Ready || state_==Publish))) && now-started_>cfg_.workSeconds*1000UL) {fail("4G 工作窗口超时");return;}
  if(state_==Probe) {
    if(ok_) {state_=Setup;next();}
    else if(error_ || now-sentAt_>timeout_) {
      if(probeResponded_) {fail("ML307 有响应但 AT 未就绪，不按电源键");return;}
      if(probeMisses_<11) {++probeMisses_;command("AT",1500);status="探测 ML307（"+std::to_string(probeMisses_)+"/12）";return;}
      if(powerPulseDone_ || powerSeenOn_) {fail("ML307 电源状态未确认，停止重复按键，按恢复策略重试");return;}
      powerPulseDone_=true;state_=Pulse;digitalWrite(5,HIGH);sentAt_=now;status="ML307 开机脉冲";
    }
  } else if(state_==Pulse && now-sentAt_>=3000) {digitalWrite(5,LOW);state_=BootWait;bootAt_=now;bootProbe_=false;}
  else if(state_==BootWait && now-bootAt_>=6000) {
    if(bootProbe_ && ok_) {state_=Setup;next();}
    else if(now-bootAt_>=30000)fail("ML307 开机后30秒内无AT响应");
    else if(!bootProbe_ || error_ || now-sentAt_>timeout_) {bootProbe_=true;command("AT",1500);status="等待 ML307 启动就绪";}
  }
  else if(state_==Setup) {
    if(step_==3) {
      if(simBlocked_) {fail("SIM 需要 PIN/PUK，请先在手机中处理");return;}
      if(error_ || (ok_ && !sim_) || now-sentAt_>timeout_) {
        if(errorCode==10)simStatus="未检测到 SIM";
        else if(errorCode==13)simStatus="SIM 通信失败，请检查触点";
        else if(errorCode==14)simStatus="SIM 忙，等待重试";
        if(!error_ || errorCode<0 || errorCode==10 || errorCode==13 || errorCode==14) {
          if(simRetries_<4) {++simRetries_;state_=SimWait;sentAt_=now;error_=ok_=false;status="等待 SIM 重试（"+std::to_string(simRetries_)+"/4）";return;}
        }
        fail("SIM 检查失败，已停止本轮重试");return;
      }
    }
    if(step_==19 && (error_ || now-sentAt_>timeout_)) {next();return;}
    if(step_==7 && (error_ || now-sentAt_>timeout_)) {clockSeen_=clockValid=false;epoch=0;next();return;}
    if(error_) {fail("AT 操作失败，请核对型号/固件/网络与 TLS");return;}
    if(step_==3 && ok_ && !sim_) {fail("SIM 未就绪");return;}
    if(step_==4 && ok_ && !registered) {if(now-sentAt_>=3000)command("AT+CEREG?");return;}
    bool connStep=control_?step_==29:step_==25;
    bool subscriptionOk=!control_ || step_!=30 || (subMid_>=0 && subMid_==subAckMid_ && subCode_>=0 && subCode_<=1);
    if(ok_ && (!connStep || connected) && (step_!=7 || clockSeen_) && subscriptionOk) {
      if(control_ && step_==30)subscribed=true;
      if(step_==2)identity=response_.c_str();
      if(step_==7) {
        timeval tv{};tv.tv_sec=static_cast<decltype(tv.tv_sec)>(epoch);
        if(settimeofday(&tv,nullptr)!=0) {clockSeen_=clockValid=false;epoch=0;}
        else clockValid=true;
      }
      next();
    } else if(now-sentAt_>timeout_)fail("AT/MQTT 响应超时");
  } else if(state_==SimWait) {
    if(simBlocked_) {fail("SIM 需要 PIN/PUK，请先在手机中处理");return;}
    if(now-sentAt_>=1000) {state_=Setup;sim_=false;errorCode=-1;command("AT+CPIN?");status="检查 SIM";}
  } else if(state_==ClearSession) {
    // An absent session may report ERROR; a real disconnect reports conn=2.
    // Both are cleanup outcomes, not failures of the upcoming connection.
    if(error_ || (ok_ && disconnectSeen_) || now-sentAt_>timeout_) {
      connected=false;errorCode=-1;state_=Setup;command("AT+MQTTCFG=\"version\",0,4");
    }
  } else if(state_==ClockZone) {
    if(error_ || now-sentAt_>timeout_) {
      if(clockOnly_) {status="在线每日校时失败：模组 UTC 时区设置失败";error_=false;state_=Ready;}
      else {state_=Setup;fail("模组 UTC 时区设置失败");}
    } else if(ok_) {
      state_=clockOnly_?SyncClock:Setup;
      command("AT+MNTP=\"ntp1.aliyun.com\",123,1,15",20000);status="4G 网络对时";
    }
  } else if(state_==Publish) {
    if(error_ || now-sentAt_>timeout_)fail("MQTT 发布失败或未收到 PUBACK");
    else if(ok_ && ack_.complete()) {published=true;status="MQTT QoS1 PUBACK 成功";state_=Ready;}
  } else if(state_==Ready && control_) {
    if(error_ || !connected) {fail("MQTT 长连接中断");return;}
    if(!readPending_ && now-lastSignal_>=60000) {lastSignal_=now;state_=Signal;command("AT+CSQ",5000);}
    else if(readPending_ || now-lastRead_>=10000) {lastRead_=now;readPending_=false;state_=ReadCheck;command("AT+MQTTREAD=0",5000);}
  } else if(state_==Signal) {
    if(ok_ || error_ || now-sentAt_>timeout_) {error_=false;state_=Ready;}
  } else if(state_==ReadCheck) {
    if(error_ || now-sentAt_>timeout_)fail("MQTT 下行查询超时");
    else if(ok_) {if(readPending_) {readPending_=false;state_=ReadMessage;command("AT+MQTTREAD=0,1",5000);}else state_=Ready;}
  } else if(state_==ReadMessage) {
    if(error_ || now-sentAt_>timeout_)fail("MQTT 下行读取失败");
    else if(ok_) {readPending_=true;state_=Ready;}
  } else if(state_==SyncClock) {
    if(error_ || now-sentAt_>timeout_) {status="在线每日校时失败";error_=false;state_=Ready;}
    else if(ok_ && clockSeen_) {timeval tv{};tv.tv_sec=static_cast<decltype(tv.tv_sec)>(epoch);clockValid=settimeofday(&tv,nullptr)==0;state_=Ready;}
  } else if(state_==PowerOff) {
    if(shutdownConfirmed) {state_=Done;connected=registered=false;}
    else if(ok_ || error_ || now-sentAt_>timeout_) {++shutdownAttempts;command("AT+MPOF=0",15000);state_=OffWait;}
  } else if(state_==OffWait) {
    if(shutdownConfirmed) {state_=Done;connected=registered=false;digitalWrite(5,LOW);}
    else if(error_ || now-sentAt_>15000) {
      if(shutdownAttempts<2) {++shutdownAttempts;command("AT+MPOF=0",15000);}
      else {shutdownError="两次AT关机未确认，物理电源状态未知";state_=Done;connected=registered=false;digitalWrite(5,LOW);}
    }
  }
}
bool Modem::publish(const std::string& t,const std::string& json,bool retain) {
  if(state_!=Ready || !connected || json.size()>bt::MaxPayload || t.size()>256 || t.find_first_of("\"\r\n")!=std::string::npos)return false;
  ack_.reset();published=false;state_=Publish;status="发布中，等待 PUBACK";
  String request=String("AT+MQTTPUB=0,\"")+t.c_str()+"\",1,"+(retain?1:0)+",0,"+json.size();
  // msg_len=0 with no message argument enters input mode (manual section 3.5).
  // An explicit empty string sends a retained tombstone without waiting for data.
  if(json.empty())request+=",\"\"";
  command(request,15000,json);return true;
}
bool Modem::pop(std::string& t,std::string& p) {if(inbox_.empty())return false;t=inbox_.front().first;p=inbox_.front().second;inbox_.pop_front();return true;}
bool Modem::syncClock() {if(!control_ || !ready())return false;startClock(true);return true;}
void Modem::shutdown() {
  if(state_==PowerOff || state_==OffWait || state_==Done || state_==Idle)return;
  digitalWrite(5,LOW);
  shutdownAttempted=true;
  payload_.clear();prompt_=false;state_=PowerOff;command("AT+MQTTDISC=0",3000);
}
void Modem::prepareSleep() {
  uart_.flush();uart_.end();
  pinMode(3,INPUT);pinMode(4,INPUT);digitalWrite(5,LOW);
}
