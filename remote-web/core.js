(function(root,factory){if(typeof module==='object'&&module.exports)module.exports=factory();else root.BTMonitor=factory()})(typeof globalThis==='object'?globalThis:this,function(){
  'use strict';
  const token=s=>typeof s==='string'&&/^[A-Za-z0-9_-]{1,40}$/.test(s);
  function profile(p){
    if(!p||typeof p.broker!=='string'||!/^(?:[a-z0-9](?:[a-z0-9-]*[a-z0-9])?\.)+[a-z0-9-]+$/i.test(p.broker)||p.broker.length>128)throw Error('请填写正确的 MQTT 服务器域名');
    if(!token(p.prefix)||p.prefix.length>32||!token(p.masterId)||p.masterId.length>32)throw Error('主题前缀和主编号格式不正确');
    const port=Number(p.wssPort||8084);if(!Number.isInteger(port)||port<1||port>65535)throw Error('WSS 端口无效');
    if(!Array.isArray(p.devices)||p.devices.length<1||p.devices.length>8)throw Error('请填写1～8台设备');
    const ids=new Set();for(const d of p.devices){if(!token(d.id)||d.id.length>32||ids.has(d.id)||!['master','slave'].includes(d.role))throw Error('设备编号须唯一，角色为主端或从端');ids.add(d.id)}
    if(p.devices.filter(d=>d.role==='master').length>1)throw Error('本监控组只允许一台主端');
    return {broker:p.broker,wssPort:port,prefix:p.prefix,masterId:p.masterId,devices:p.devices.map(d=>({id:d.id,role:d.role,label:String(d.label||d.id).slice(0,40)}))};
  }
  const base=p=>`btclone/${p.prefix}/${p.masterId}`;
  const topic=(p,id,leaf)=>`${base(p)}/devices/${id}/${leaf}`;
  function minutes(text,end=false){if(text==='24:00'&&end)return 1440;if(!/^(?:[01]\d|2[0-3]):[0-5]\d$/.test(text))throw Error('时间请填写 HH:MM，不支持跨午夜');const [h,m]=text.split(':').map(Number);return h*60+m}
  const hhmm=n=>`${String(Math.floor(n/60)).padStart(2,'0')}:${String(n%60).padStart(2,'0')}`;
  function slots(a){
    if(!Array.isArray(a)||a.length>5)throw Error('每天最多5个时段');
    const out=a.map(s=>({start:typeof s.start==='string'?minutes(s.start):s.start,end:typeof s.end==='string'?minutes(s.end,true):s.end}));
    for(let i=0;i<out.length;i++){const a=out[i];if(!Number.isInteger(a.start)||!Number.isInteger(a.end)||a.start<0||a.start>=1440||a.end>1440||a.start>=a.end)throw Error('开始须早于结束，不支持跨午夜');for(let j=0;j<i;j++)if(a.start<out[j].end&&out[j].start<a.end)throw Error('工作时段不能重叠')}
    return out;
  }
  function view(state,presence,now=Math.floor(Date.now()/1000),connected=true){
    if(!state)return {label:'尚未上报',tone:'muted',detail:'等待设备首次联网'};
    if(!connected)return {label:'网页连接中断',tone:'warn',detail:'以下为最后收到的状态'};
    const age=now-Number(state.reported_at||0),wake=Number(state.next_wake_at||0);
    if(state.planned_sleep&&wake>0){if(now<=wake+120)return {label:'预计深睡',tone:'sleep',detail:`计划唤醒 ${date(wake)}`};return {label:'唤醒超时',tone:'warn',detail:'未按计划上报，状态未知'}}
    if(presence==='offline'||(presence&&typeof presence==='object'&&presence.online===false))return {label:'连接中断',tone:'warn',detail:'设备遗嘱消息，具体原因待补报'};
    if(!Number.isFinite(age)||!state.reported_at||age>150||age< -300)return {label:'状态未知',tone:'warn',detail:'近期未收到有效状态上报'};
    return {label:'在线',tone:'online',detail:`最近上报 ${date(state.reported_at)}`};
  }
  function date(epoch){const d=new Date(epoch*1000);return Number.isFinite(epoch)&&epoch>0&&Number.isFinite(d.getTime())?new Intl.DateTimeFormat('zh-CN',{timeZone:'Asia/Shanghai',month:'2-digit',day:'2-digit',hour:'2-digit',minute:'2-digit',second:'2-digit',hour12:false}).format(d):'时间未知'}
  function countdown(epoch,now=Math.floor(Date.now()/1000)){if(!epoch)return '尚未安排';let s=Math.max(0,Math.ceil(epoch-now));return `${Math.floor(s/3600)}小时 ${Math.floor(s%3600/60)}分 ${s%60}秒`}
  function command(p,d,action,patch,revision,state,now=Math.floor(Date.now()/1000),id){
    if(!['config','restart',...(d.role==='master'?['run','battery-full']:[])].includes(action))throw Error('当前设备不支持此操作');
    const expiry=Math.min(now+7*86400,Math.max(now+3600,Number(state?.next_wake_at||0)+7200));
    if(!id){const b=new Uint8Array(16);globalThis.crypto.getRandomValues(b);id=Array.from(b,n=>n.toString(16).padStart(2,'0')).join('')}
    if(!token(id))throw Error('指令编号无效');
    const payload={v:1,id,device_id:d.id,action,issued_at:now,expires_at:expiry};
    if(action==='config'){if(!Number.isInteger(revision)||revision<1)throw Error('请先等设备上报配置版本');if(!patch||typeof patch!=='object'||Array.isArray(patch))throw Error('参数无效');if(d.role==='slave'&&patch.sleep)throw Error('从端固定常在线');if(patch.slots)patch={...patch,slots:slots(patch.slots)};payload.expected_revision=revision;payload.patch=patch}
    const json=JSON.stringify(payload);if(new TextEncoder().encode(json).length>1024)throw Error('单条修改过多，请分批保存');
    return {topic:topic(p,d.id,`cmd/${action}/${id}`),payload,json};
  }
  return {profile,base,topic,slots,minutes,hhmm,view,date,countdown,command};
});
