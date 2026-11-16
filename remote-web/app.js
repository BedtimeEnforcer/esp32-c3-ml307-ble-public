/* Credentials remain in the MQTT connection's memory. No password or device
   data is written to localStorage, URL parameters, analytics or a server. */
(function(){
  'use strict';
  const C=window.BTMonitor,$=s=>document.querySelector(s),el=(tag,text,cls)=>{const e=document.createElement(tag);if(text!==undefined)e.textContent=text;if(cls)e.className=cls;return e};
  const form=$('#settings'),notice=$('#notice');
  let profile,client,connected=false,loggedIn=false,stopped=true,attempts=0,retryTimer,healthyTimer,loginTimer,session=0;
  let devices=new Map(),selected='',loadedRevision=0,dirty=false,pending=new Map(),latestRaw=null;
  const labels={config:'保存设置',run:'扫描并上传',restart:'重启设备','battery-full':'满充重置'};
  const states={received:'已接收',running:'正在执行',done:'已执行',failed:'执行失败',expired:'已过期',pending:'待设备接收',publishing:'正在提交',unconfirmed:'服务器确认未知'};
  const advanced=[
    ['workSeconds','单次联网 / 手动任务上限（秒）',60,600,'master'],['retryAttempts','每批统计次数',1,5],['retryBaseSeconds','初次退避（秒）',10,3600],['retryMaxSeconds','最大退避（秒）',10,7200],['connectSeconds','连接尝试上限（秒）',10,120],['recoveryRestart','允许有限自动重启','checkbox'],['restartBatches','重启前失败批次',1,10],['screenSeconds','亮屏时长（秒）',5,600],['staleSeconds','缓存过期提示（秒）',60,31536000],['targetMac','目标 / 模拟 MAC','text'],['generation','数据代次 generation',1,4294967295],['simulate','启用蓝牙广播','checkbox',null,'slave'],['fixed','固定模式','checkbox',null,'slave'],['fixedAdv','固定 ADV（十六进制）','text',null,'slave'],['fixedRsp','固定 RSP（十六进制）','text',null,'slave'],['fixedPdu','固定 PDU','select',null,'slave'],['strict','MBeacon 完整性校验','checkbox'],['rollOffset','滚动码起始字节',0,61],['rollLength','滚动码字节数',0,8],['rollMirror','镜像校验偏移（−1关闭）',-1,61],['batteryMode','开启电量估算','checkbox',null,'master'],['usableMah','5V侧可用容量（mAh）',100,10000,'master'],['sleepMa','估算睡眠电流（mA）',0,100,'master'],['activeMa','估算活动电流（mA）',1,500,'master'],['modemMa','估算蜂窝额外电流（mA）',1,2000,'master']
  ];
  for(const [name,label,min,max,role]of advanced){const l=el('label',label);if(role)l.className=role+'-control';let i;if(min==='select'){i=el('select');for(const p of ['ADV_IND','ADV_SCAN_IND','ADV_NONCONN_IND']){const o=el('option',p);o.value=p;i.append(o)}}else{i=el('input');i.type=min==='checkbox'?'checkbox':min==='text'?'text':'number';if(i.type==='number'){i.min=min;i.max=max;i.step=['sleepMa','activeMa','modemMa'].includes(name)?'0.1':'1'}}i.name=name;l.append(i);$('#advanced-fields').append(l)}
  function tell(s){notice.textContent=s}
  function device(){return devices.get(selected)}
  function readProfile(){const ds=$('#device-list').value.trim().split(/\r?\n/).filter(Boolean).map(line=>{let [id,role,label]=line.split(',').map(v=>v.trim());return {id,role,label}});return C.profile({broker:$('#broker').value.trim(),wssPort:Number($('#port').value),prefix:$('#prefix').value.trim(),masterId:$('#master-id').value.trim(),devices:ds})}
  function fillProfile(p){p=C.profile(p);$('#broker').value=p.broker;$('#port').value=p.wssPort;$('#prefix').value=p.prefix;$('#master-id').value=p.masterId;$('#device-list').value=p.devices.map(d=>[d.id,d.role,d.label].join(',')).join('\n')}
  try{const saved=localStorage.getItem('btclone.connection.v1');if(saved)fillProfile(JSON.parse(saved))}catch{}
  $('#profile-file').onchange=async e=>{try{const f=e.target.files[0];if(!f)return;if(f.size>10000)throw Error('连接资料文件过大');fillProfile(JSON.parse(await f.text()));tell('连接资料已导入，请填写专用网页账号和密码。')}catch(e){tell(e.message)}finally{e.target.value=''}};
  function wipe(){devices=new Map();pending=new Map();latestRaw=null;selected='';loadedRevision=0;dirty=false;$('#devices').replaceChildren();$('#commands').replaceChildren();$('#history').replaceChildren();$('#slots').replaceChildren();$('#raw').textContent='等待主端数据';$('#raw-time').textContent='尚未收到有效快照';form.reset();$('#selected-device').replaceChildren();$('#settings-fields').disabled=true;$('#config-message').textContent='等待配置上报'}
  function stop(message='已退出，设备仍按原配置工作。'){
    stopped=true;session++;connected=loggedIn=false;clearTimeout(retryTimer);clearTimeout(healthyTimer);clearTimeout(loginTimer);
    if(client){const old=client;client=null;old.end(true);if(old.options){old.options.password=undefined;old.options.username=undefined}}
    $('#password').value='';$('#dashboard').hidden=true;$('#login-view').hidden=false;$('#logout').hidden=true;$('#login-button').disabled=false;wipe();tell(message);
  }
  $('#logout').onclick=()=>stop();
  // One authorized device subtree covers state/config/history/receipts/commands.
  // Two devices use three filters; even the supported maximum of eight uses nine.
  function subscribeTopics(p){return [`${C.base(p)}/raw`,...p.devices.map(d=>C.topic(p,d.id,'#'))]}
  $('#login-form').onsubmit=e=>{
    e.preventDefault();if(!stopped)return;
    try{
      profile=readProfile();const username=$('#username').value,password=$('#password').value;if(!username||!password)throw Error('请填写网页账号和密码');
      if(!window.mqtt)throw Error('MQTT组件未加载，请重新打开网页');
      wipe();for(const d of profile.devices)devices.set(d.id,{...d,state:null,presence:null,configs:new Map(),hist:new Map(),acks:new Map()});selected=profile.devices[0].id;
      for(const d of profile.devices){const o=el('option',d.label);o.value=d.id;$('#selected-device').append(o)}
      stopped=false;attempts=0;connected=loggedIn=false;const current=++session;$('#login-button').disabled=true;tell('正在通过加密连接验证账号…');
      client=mqtt.connect(`wss://${profile.broker}:${profile.wssPort}/mqtt`,{username,password,clientId:'web-'+crypto.randomUUID(),protocolVersion:4,clean:true,reconnectPeriod:0,connectTimeout:10000,keepalive:30});
      client.on('connect',()=>{
        if(session!==current||stopped)return;
        client.subscribe(subscribeTopics(profile),{qos:1},(error,granted)=>{
          if(session!==current||stopped)return;
          if(error||!Array.isArray(granted)||granted.length!==subscribeTopics(profile).length||granted.some(g=>g.qos>1)){stop('账号认证或读取权限不足，请按本地配置单核对网页账号权限。');return}
          clearTimeout(loginTimer);connected=loggedIn=true;$('#login-view').hidden=true;$('#dashboard').hidden=false;$('#logout').hidden=false;$('#password').value='';$('#connection').textContent='MQTT 已连接';$('#connection').className='pill online';
          try{localStorage.setItem('btclone.connection.v1',JSON.stringify(profile))}catch{}
          tell('已连接。实时接收状态；深睡设备的指令将在下次联网执行。');render();loadConfig(false);
          clearTimeout(healthyTimer);healthyTimer=setTimeout(()=>{if(session===current&&connected)attempts=0},60000);
        });
      });
      client.on('message',(t,p)=>{if(session===current&&!stopped)receive(t,p.toString())});
      client.on('error',()=>{if(session===current&&!stopped)tell('连接或认证未成功，正在按有限重连规则处理。')});
      client.on('close',()=>{
        if(session!==current||stopped)return;connected=false;clearTimeout(healthyTimer);clearTimeout(loginTimer);
        $('#connection').textContent='网页连接中断';$('#connection').className='pill warn';render();
        if(++attempts>3){if(!loggedIn)stop('登录未成功，已停止自动重试。请核对域名、账号、密码及WSS权限。');else tell('自动重连已尝试3次并停止。保留最后状态；请退出后重新登录。');return}
        tell(`连接中断，将进行第${attempts}/3次重连。`);retryTimer=setTimeout(()=>{if(session===current&&!stopped)client.reconnect()},2000*2**(attempts-1));
      });
      loginTimer=setTimeout(()=>{if(session===current&&!loggedIn)stop('连接或订阅确认超时，已停止本次登录。')},45000);
    }catch(e){stop(e.message)}
  };
  function receive(t,p){
    if(t===`${C.base(profile)}/raw`){try{const d=JSON.parse(p);if(d.v===1&&d.device_id===profile.masterId&&typeof d.adv_hex==='string'&&typeof d.scan_rsp_hex==='string'&&/^[0-9a-f]*$/i.test(d.adv_hex+d.scan_rsp_hex)&&d.adv_hex.length<=62&&d.scan_rsp_hex.length<=62){latestRaw=d;renderRaw()}}catch{}return}
    for(const d of devices.values()){
      const root=C.topic(profile,d.id,'');if(!t.startsWith(root))continue;const leaf=t.slice(root.length);let value;
      if(leaf.startsWith('cmd/')){let id=leaf.split('/')[2];if(!p){pending.delete(d.id+'/'+id);renderCommands();return}try{value=JSON.parse(p);if(value.id!==id||value.device_id!==d.id)return;pending.set(d.id+'/'+id,{...value,device:d.id,state:'pending'});renderCommands()}catch{}return}
      if(leaf==='presence'){try{d.presence=JSON.parse(p)}catch{d.presence=p}renderDevices();return}
      try{value=JSON.parse(p)}catch{return}
      if(!value||typeof value!=='object'||Array.isArray(value))return;
      if(leaf==='state'&&value.v===1&&value.device_id===d.id){d.state=value;renderDevices()}
      else if(leaf==='diagnostics'&&value.v===1&&value.device_id===d.id){d.diagnostics=value;renderDevices()}
      else if(/^config\/[01]$/.test(leaf)&&value.v===1&&value.device_id===d.id&&Number.isInteger(value.revision)){d.configs.set(Number(leaf.at(-1)),value);if(selected===d.id)loadConfig(false)}
      else if(/^history\/[0-3]$/.test(leaf)&&Array.isArray(value.items)){d.hist.set(leaf,value.items.slice(0,3));renderHistory()}
      else if(/^ack\/(?:[0-7]|rejected)$/.test(leaf)&&Array.isArray(value.items)){d.acks.set(leaf,value.items.slice(0,2));renderCommands()}
      return;
    }
  }
  function metric(host,label,value){const m=el('div',undefined,'metric');m.append(el('small',label),el('strong',value));host.append(m)}
  function renderDevices(){
    $('#devices').replaceChildren();for(const d of devices.values()){
      const s=d.state||{},v=C.view(d.state,d.presence,undefined,connected),card=el('article',undefined,'card device');const head=el('div',undefined,'device-head'),name=el('div');name.append(el('div',d.role==='master'?'MASTER · 主模块':'SLAVE · 从模块','eyebrow'),el('h2',d.label),el('div',d.id,'device-code'));head.append(name,el('span',v.label,'pill '+v.tone));card.append(head,el('div',s.rolling||'— — — —','rolling'),el('div','滚动码 · '+(s.changed_at?C.date(s.changed_at)+' 变化':'尚无有效变化时间'),'fine'));
      const metrics=el('div',undefined,'metrics');metric(metrics,'当前连接',v.detail);metric(metrics,'下次工作',d.role==='master'?C.countdown(s.next_window_at):'持续在线 · 实时接收');metric(metrics,'无线信号',d.role==='master'?s.cell_rssi!=null?s.cell_rssi+' dBm':'4G 信号未知':s.wifi_rssi!=null?s.wifi_rssi+' dBm':'WiFi 未连接');metric(metrics,'MQTT / 网络',d.state?(s.mqtt?'MQTT 已连接':'MQTT 未连接')+' · '+(d.role==='master'?(s.cell_registered?'4G 已注网':'4G 未注网'):(s.wifi?'WiFi 已连接':'WiFi 未连接')):'尚未上报');metric(metrics,'目标 BLE 信号',s.ble_rssi!=null?s.ble_rssi+' dBm':'尚未采集');metric(metrics,'电量 / 供电',s.battery_est_pct!=null?s.battery_est_pct+'%（估算）':d.role==='slave'?'外接电源':'尚未启用估算');metric(metrics,'蓝牙 / 运行模式',(s.ble_active?'工作中':'当前未工作')+' · '+(s.mode==='fixed'?'固定广播':d.role==='master'?'采集':'跟随主端'));metric(metrics,'数据采集时间',C.date(s.observed_at));metric(metrics,'最近执行',s.result_at?(s.result_ok?'成功':'失败')+' · '+(s.result_reason||''):'尚无结果');card.append(metrics);
      if(d.diagnostics){const t=d.diagnostics;metric(metrics,'固件 / 配置',(t.firmware||'未知')+' · '+(t.config_status||'未知'));if(d.role==='master')metric(metrics,'SIM / 模组',(t.sim||'未检查')+' · '+(t.modem_status||'未知'));card.append(el('p','诊断更新 '+C.date(t.reported_at),'fine'))}
      if(s.shutdown_fault)card.append(el('p','模组关机未确认，物理电源状态未知。','fine'));
      const actions=el('div',undefined,'actions');for(const a of d.role==='master'?['run','restart','battery-full']:['restart']){const b=el('button',labels[a],'quiet');b.type='button';b.disabled=!connected;b.onclick=()=>send(d,a);actions.append(b)}card.append(actions);$('#devices').append(card);
    }
    $('#settings-fields').disabled=!connected||!configReady(device());
  }
  function configReady(d){if(!d)return false;const a=d.configs.get(0),b=d.configs.get(1);return a&&b&&a.revision===b.revision}
  function addSlot(a={start:480,end:495}){if($('#slots').children.length>=5){tell('每天最多5个时段。');return}const r=el('div',undefined,'slot');for(const k of ['start','end']){const i=el('input');i.value=C.hhmm(a[k]);i.dataset.slot=k;i.placeholder='08:00';i.setAttribute('aria-label',k==='start'?'时段开始':'时段结束');r.append(i);if(k==='start')r.append(el('span','至'))}const b=el('button','删除','quiet');b.type='button';b.onclick=()=>{r.remove();dirty=true};r.append(b);$('#slots').append(r)}
  $('#add-slot').onclick=()=>{addSlot();dirty=true};form.oninput=()=>{dirty=true};
  function loadConfig(force){
    const d=device();if(!configReady(d)){$('#config-message').textContent='等待设备上报完整配置（主端深睡时需等下次联网）。';$('#settings-fields').disabled=true;return}
    const a=d.configs.get(0),b=d.configs.get(1);$('#settings-fields').disabled=!connected;
    if(!force&&dirty){if(a.revision!==loadedRevision)$('#config-message').textContent='设备参数已在其他入口更新。当前编辑保留，保存可能发生版本冲突。';return}
    if(!force&&loadedRevision===a.revision)return;
    const values={...a.values,...b.values};for(const e of form.elements){if(!e.name||values[e.name]===undefined)continue;if(e.type==='checkbox')e.checked=values[e.name];else e.value=values[e.name]}
    $('#slots').replaceChildren();for(const s of values.slots||[])addSlot(s);loadedRevision=a.revision;dirty=false;
    document.querySelectorAll('.master-control').forEach(e=>e.hidden=d.role!=='master');document.querySelectorAll('.slave-control').forEach(e=>e.hidden=d.role!=='slave');
    $('#config-message').textContent=`${d.label} · 配置版本 ${loadedRevision}。保存后等待设备回执；远程配置不会解锁本地屏幕。`;
  }
  $('#selected-device').onchange=e=>{if(dirty&&!confirm('切换设备将放弃尚未保存的编辑，继续？')){e.target.value=selected;return}selected=e.target.value;dirty=false;loadedRevision=0;loadConfig(true)};
  $('#reload-config').onclick=()=>{if(dirty&&!confirm('放弃尚未保存的编辑，重新读取已上报参数？'))return;dirty=false;loadConfig(true)};
  form.onsubmit=e=>{e.preventDefault();if(!connected)return;const d=device(),patch={};try{for(const i of form.elements){if(!i.name||i.closest('[hidden]'))continue;if(i.type==='checkbox')patch[i.name]=i.checked;else if(i.type==='number'){if(i.value==='')throw Error('请填完数值参数');patch[i.name]=Number(i.value)}else patch[i.name]=i.value}if(d.role==='master')patch.slots=C.slots(Array.from($('#slots').children).map(r=>({start:r.querySelector('[data-slot=start]').value,end:r.querySelector('[data-slot=end]').value})));else delete patch.sleep;
      // Send only edited fields; reduces traffic and command size.
      const values={...d.configs.get(0).values,...d.configs.get(1).values};for(const k of Object.keys(patch))if(JSON.stringify(patch[k])===JSON.stringify(values[k]))delete patch[k];
      if(!Object.keys(patch).length){tell('没有需要保存的修改。');return}if(('targetMac'in patch||'generation'in patch)&&!confirm('将修改蓝牙来源或数据代次，请同步其他相关设备。继续？'))return;send(d,'config',patch);
    }catch(e){tell(e.message)}};
  function send(d,action,patch){
    if(!connected){tell('网页未连接，未提交指令。');return}
    if(action!=='config'&&!confirm(`${labels[action]}：${d.label}？${d.state?.planned_sleep?'设备预计深睡，将在下次联网处理。':''}`))return;
    try{
      const count=Array.from(pending.values()).filter(x=>x.device===d.id&&x.expires_at>Date.now()/1000).length;if(count>=8)throw Error('该设备已有8条待办，请等待设备处理');
      const cmd=C.command(profile,d,action,patch,loadedRevision,d.state);pending.set(d.id+'/'+cmd.payload.id,{...cmd.payload,device:d.id,state:'publishing'});renderCommands();
      client.publish(cmd.topic,cmd.json,{qos:1,retain:true},error=>{const key=d.id+'/'+cmd.payload.id,item=pending.get(key);if(item)item.state=error?'unconfirmed':'pending';if(action==='config'&&!error){dirty=false;loadConfig(true)}renderCommands();tell(error?'未取得服务器确认。不要连续重发，先查看指令结果。':'服务器已保存指令，等待设备接收及执行回执。')});
    }catch(e){tell(e.message)}
  }
  function row(tbody,values){const tr=el('tr');for(const v of values)tr.append(el('td',v));tbody.append(tr)}
  function renderCommands(){const table=$('#commands');table.replaceChildren();const all=new Map(pending);for(const d of devices.values())for(const entries of d.acks.values())for(const r of entries){const key=d.id+'/'+r.id;all.set(key,{...r,device:d.id})}
    const sorted=Array.from(all.values()).sort((a,b)=>(b.at||b.issued_at||0)-(a.at||a.issued_at||0)).slice(0,64);for(const r of sorted){const expired=['pending','publishing','unconfirmed'].includes(r.state)&&r.expires_at<Date.now()/1000;row(table,[`${devices.get(r.device)?.label||r.device} / ${labels[r.action]||r.action}`,states[expired?'expired':r.state]||r.state,C.date(r.at||r.issued_at),r.reason||(expired?'超过有效期，不应继续执行':'等待设备回执')])}if(!sorted.length)row(table,['尚无指令','—','—','设备接收后会在这里显示结果'])}
  function renderHistory(){const table=$('#history');table.replaceChildren();const all=[];for(const d of devices.values())for(const items of d.hist.values())for(const h of items)all.push({...h,label:d.label});all.sort((a,b)=>b.at-a.at);for(const h of all)row(table,[h.label,h.ok?'成功':'失败',C.date(h.at),h.reason]);if(!all.length)row(table,['尚无历史','—','—','设备保存记录，联网后补报'])}
  function renderRaw(){if(!latestRaw)return;$('#raw-time').textContent=`采集 ${C.date(latestRaw.observed_at)} · 变化 ${C.date(latestRaw.changed_at)} · 上传 ${C.date(latestRaw.reported_at)}`;$('#raw').textContent=`MAC   ${latestRaw.mac}\nPDU   ${latestRaw.adv_type}\nADV   ${latestRaw.adv_hex}\nRSP   ${latestRaw.scan_rsp_hex}\nRAW   ${latestRaw.adv_hex}${latestRaw.scan_rsp_hex}`}
  function render(){renderDevices();renderCommands();renderHistory();renderRaw()}
  setInterval(()=>{$('#clock').textContent=C.date(Math.floor(Date.now()/1000))+' 北京时间';if(loggedIn){renderDevices();renderCommands()}},1000);
  // Deliberately no public demo-data switch and no credentials in a URL.
})();
