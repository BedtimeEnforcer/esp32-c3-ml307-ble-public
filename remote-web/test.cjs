// Actual production JS with mocked broker callbacks. Never opens a connection.
const fs=require('node:fs'),path=require('node:path'),assert=require('node:assert/strict'),{EventEmitter}=require('node:events');
let JSDOM;try {({JSDOM}=require('jsdom'));}catch {({JSDOM}=require(path.resolve(__dirname,'../.dev-evidence/web-test/node_modules/jsdom')));}
const C=require('./core.js');let checks=0;
function check(v,m){assert.ok(v,m);checks++}
const p=C.profile({broker:'mqtt.example.com',prefix:'pair-test',masterId:'master-01',devices:[{id:'master-test',role:'master',label:'主模块'},{id:'slave-test',role:'slave',label:'从模块'}]});
const now=1794355200;
check(C.minutes('08:15')===495,'Beijing HH:MM');check(C.minutes('24:00',true)===1440,'midnight end');
for(const s of ['24:00','8:00','25:00','23:60','08:00x']){assert.throws(()=>C.minutes(s));checks++}
check(C.slots([{start:480,end:495},{start:1225,end:1235}]).length===2,'two work windows');
for(const a of [[{start:1380,end:60}],[{start:480,end:480}],[{start:480,end:495},{start:490,end:510}],Array(6).fill({start:1,end:2})]){assert.throws(()=>C.slots(a));checks++}
check(C.slots([]).length===0,'zero schedules allowed');
check(C.view(null,null,now).label==='尚未上报','no fabricated data');
check(C.view({planned_sleep:true,next_wake_at:now+600,reported_at:now-3600},'offline',now).label==='预计深睡','sleep instead of offline');
check(C.view({planned_sleep:true,next_wake_at:now-121},'offline',now).label==='唤醒超时','stale sleep not indefinitely healthy');
check(C.view({reported_at:now-151},null,now).label==='状态未知','stale telemetry');
check(C.view({reported_at:'invalid'},null,now).label==='状态未知','invalid timestamp never fabricates online status');
check(C.date('invalid')==='时间未知'&&C.date(1e20)==='时间未知','invalid dates do not break rendering');
check(C.view({reported_at:now},'online',now).label==='在线','fresh state');
check(C.view({reported_at:now},'offline',now).label==='连接中断','last will');
check(C.view({reported_at:now},'online',now,false).label==='网页连接中断','browser offline explicit');
assert.throws(()=>C.profile({...p,broker:'evil.invalid/path'}));checks++;assert.throws(()=>C.profile({...p,devices:[p.devices[0],p.devices[0]]}));checks++;
const cmd=C.command(p,p.devices[0],'run',null,1,{next_wake_at:now+12*3600},now,'test-id');check(cmd.payload.expires_at>now+12*3600,'offline command survives next wake');check(cmd.topic.endsWith('/cmd/run/test-id'),'per-command retained topic');
assert.throws(()=>C.command(p,p.devices[1],'run',null,1,{},now,'test-id'));checks++;
assert.throws(()=>C.command(p,p.devices[1],'config',{sleep:true},1,{},now,'test-id'));checks++;
assert.throws(()=>C.command(p,p.devices[0],'config',{scanEvery:1},0,{},now,'test-id'));checks++;
function fixture(){
  const dom=new JSDOM(fs.readFileSync(path.join(__dirname,'index.html'),'utf8'),{url:'https://example.invalid/',runScripts:'outside-only'}),w=dom.window;
  w.TextEncoder=TextEncoder;Object.defineProperty(w.crypto,'randomUUID',{value:require('node:crypto').randomUUID});w.confirm=()=>true;w.setInterval=()=>1;w.clearInterval=()=>{};w.setTimeout=()=>1;w.clearTimeout=()=>{};
  const instances=[];
  class Client extends EventEmitter{constructor(url,options){super();this.url=url;this.options=options;this.published=[]}subscribe(topics,options,callback){this.topics=topics;callback(null,topics.map(topic=>({topic,qos:this.refuse?128:1})))}publish(t,json,o,cb){this.published.push({t,json,o});cb(null)}end(){this.ended=true}reconnect(){this.reconnected=true}}
  w.mqtt={connect:(url,opts)=>{const c=new Client(url,opts);instances.push(c);return c}};
  for(const f of ['core.js','app.js'])w.eval(fs.readFileSync(path.join(__dirname,f),'utf8'));
  const $=s=>w.document.querySelector(s);$('#broker').value=p.broker;$('#prefix').value=p.prefix;$('#master-id').value=p.masterId;$('#device-list').value='master-test,master,主模块\nslave-test,slave,从模块';$('#username').value='web-test';$('#password').value='private-test-password';
  return {dom,w,$,instances,login(){ $('#login-form').onsubmit({preventDefault(){}});return instances.at(-1)}};
}
const config={sleep:true,slots:[{start:480,end:495},{start:1225,end:1235}],scanEvery:86400,scanSeconds:20,uploadEvery:86400,workSeconds:180,retryAttempts:3,retryBaseSeconds:60,retryMaxSeconds:900,cooldownSeconds:21600,connectSeconds:30,recoveryRestart:false,restartBatches:3,screenSeconds:30,staleSeconds:1209600,targetMac:'C0:11:22:33:44:55',generation:1,simulate:true,fixed:false,fixedAdv:'',fixedRsp:'',fixedPdu:'ADV_IND',strict:true,rollOffset:25,rollLength:4,rollMirror:42,batteryMode:true,usableMah:1800,sleepMa:5,activeMa:60,modemMa:200};
function report(c,d,leaf,data){c.emit('message',C.topic(p,d,leaf),Buffer.from(typeof data==='string'?data:JSON.stringify(data)))}
function configs(c,id='master-test',revision=1,extra={}){for(let page=0;page<2;page++)report(c,id,`config/${page}`,{v:1,device_id:id,revision,page,values:{...config,...extra}})}
{
  const {dom,w,$,login}=fixture();check($('#dashboard').hidden,'data hidden before auth');check($('#devices').children.length===0,'no unauthenticated data');const c=login();check(c.url==='wss://mqtt.example.com:8084/mqtt','only secure websocket');check(c.options.password==='private-test-password','password only runtime connection');check(c.options.reconnectPeriod===0,'no infinite reconnect');check($('#dashboard').hidden,'connection is not authentication');c.emit('connect');
  check(!$('#dashboard').hidden&&$('#login-view').hidden,'visible after granted subscriptions');check($('#password').value==='','password input cleared');check(!w.localStorage.getItem('btclone.connection.v1').includes('password'),'profile does not store credentials');
  check(c.topics.every(t=>t.startsWith(C.base(p)+'/')),'subscription limited to configured pair');
  check(c.topics.length===3&&c.topics.includes(C.topic(p,'master-test','#'))&&c.topics.includes(C.topic(p,'slave-test','#')),'two devices stay below the observed subscription budget');
  report(c,'master-test','state',null);report(c,'master-test','state',[]);check($('#devices').children.length===2,'malformed MQTT values do not break the dashboard');
  report(c,'master-test','diagnostics',{v:1,device_id:'master-test',reported_at:now,firmware:'0.3.6-dev',sim:'SIM 已就绪',config_status:'已加载有效配置',modem_status:'MQTT 已连接'});
  check($('#devices').textContent.includes('SIM 已就绪')&&$('#devices').textContent.includes('0.3.6-dev'),'real device diagnostics rendered separately from online status');
  check($('#settings-fields').disabled,'wait for device configuration');configs(c);check(!$('#settings-fields').disabled,'two matching config pages ready');check($('#slots').children.length===2,'schedule loaded');
  for(let i=0;i<5;i++)$('#add-slot').onclick();check($('#slots').children.length===5,'sixth slot refused');$('#reload-config').onclick();check($('#slots').children.length===2,'reload discards edit with confirm');
  $('#settings [name=scanSeconds]').value='25';$('#settings').oninput();$('#settings').onsubmit({preventDefault(){}});let sent=JSON.parse(c.published.at(-1).json);check(sent.action==='config'&&sent.expected_revision===1,'configuration optimistic concurrency');check(sent.patch.scanSeconds===25&&Object.keys(sent.patch).length===1,'only changed parameter sent');check(c.published.at(-1).o.retain&&c.published.at(-1).o.qos===1,'commands queued with QoS1 retain');check($('#commands').textContent.includes('待设备接收'),'server acknowledgement not execution');
  report(c,'master-test','ack/0',{items:[{id:sent.id,action:'config',state:'done',at:now,reason:'saved'}]});check($('#commands').textContent.includes('已执行'),'device execution receipt');
  report(c,'master-test','history/0',{items:[{at:now,ok:false,reason:'<img src=x onerror=alert(1)>'}]});check(!w.document.querySelector('img'),'history safely escaped');check($('#history').textContent.includes('<img'),'reason as literal text');
  c.emit('message',C.base(p)+'/raw',Buffer.from(JSON.stringify({v:1,device_id:p.masterId,adv_hex:'020106',scan_rsp_hex:'',mac:'C0:11:22:33:44:55',adv_type:'ADV_IND',observed_at:now,changed_at:now,reported_at:now})));check($('#raw').textContent.includes('020106'),'legacy raw still readable');
  configs(c,'slave-test',2,{sleep:false});$('#selected-device').value='slave-test';$('#selected-device').onchange({target:$('#selected-device')});check($('#settings [name=sleep]').closest('.master-control').hidden,'slave has no sleep switch');
  const before=c.published.length;w.confirm=()=>false;$('#devices').querySelector('button').onclick();check(c.published.length===before,'cancel command has no effect');w.confirm=()=>true;
  c.emit('close');check($('#settings-fields').disabled,'browser disconnect disables controls');check($('#connection').textContent.includes('中断'),'browser connection state explicit');
  $('#logout').onclick();check($('#dashboard').hidden&&$('#devices').children.length===0,'logout wipes device data');check(c.options.password===undefined,'logout releases password reference');check($('#raw').textContent==='等待主端数据','logout clears raw');dom.window.close();
}
{
  const {dom,$,login}=fixture();const c=login();c.refuse=true;c.emit('connect');check($('#dashboard').hidden,'ACL denial keeps data closed');check($('#notice').textContent.includes('权限不足'),'permission denial actionable');dom.window.close();
}
console.log(`PASS ${checks} remote web validation/DOM/auth/config/command/privacy checks (MQTT mocked)`);
