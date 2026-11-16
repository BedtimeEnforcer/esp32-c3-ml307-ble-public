// Runs the real embedded page with a DOM and mocked HTTP responses. No device
// or live credentials are involved. Tests the configuration serialization flow.
const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');
const root = path.resolve(__dirname, '..');
let JSDOM;try {({JSDOM}=require('jsdom'));}catch {({JSDOM}=require(path.resolve(root, '../.dev-evidence/web-test/node_modules/jsdom')));}
let checks = 0;
function check(value, message) {assert.ok(value, message);checks++;}
async function test(role) {
  let requests = [], failSave = false, queuedConfig=null;
  const config = {
    csrfToken:'test-nonce',revision:1,slots:[{start:480,end:495},{start:1225,end:1235}],deviceId:role + '-test',masterId:'master-01',prefix:'pair-test',targetMac:'C0:11:22:33:44:55',generation:1,
    wifiSsid:'Test AP',broker:'mqtt.example.com',mqttUser:'test',caPem:'',apn:'',modemCa:'btclone-ca',
    tls:true,labPlaintext:false,fixed:false,sleep:false,batteryMode:false,strict:true,simulate:true,port:8883,
    rollOffset:25,rollLength:4,rollMirror:42,scanEvery:86400,scanSeconds:20,uploadEvery:86400,workSeconds:180,
    displayLock:false,recoveryRestart:false,retryAttempts:3,retryBaseSeconds:60,retryMaxSeconds:900,cooldownSeconds:21600,connectSeconds:30,restartBatches:3,apSeconds:300,maintenanceBootSeconds:120,screenSeconds:30,staleSeconds:1209600,usableMah:1800,sleepMa:5,activeMa:60,modemMa:200,
    fixedAdv:'',fixedRsp:'',fixedPdu:'ADV_IND',wifiPasswordSet:true,mqttPasswordSet:true
  };
  const status = {configRevision:1,binding:{masterId:config.masterId,prefix:config.prefix,targetMac:config.targetMac,generation:config.generation},role,deviceId:config.deviceId,rolling:'11223344',cacheState:'已缓存',nearby:[],logs:['<img src=x onerror=alert(1)>']};
  let html = fs.readFileSync(path.join(root,'web/index.html'),'utf8').replaceAll('__CSRF__','test-nonce').replaceAll('__ROLE__',role);
  const script = html.match(/<script>([\s\S]*?)<\/script>/)[1];
  const dom = new JSDOM(html,{url:'http://192.168.4.1/',runScripts:'outside-only'});
  dom.window.setInterval = () => 1;
  dom.window.confirm = () => true;
  let downloaded=false;dom.window.URL.createObjectURL=()=> 'blob:test-backup';dom.window.URL.revokeObjectURL=()=>{};
  dom.window.HTMLAnchorElement.prototype.click=function(){downloaded=this.download.endsWith('.json')};
  dom.window.fetch = async (url, options) => {
    requests.push({url,options});
    let data = url.endsWith('/status')?status:url.endsWith('/config') && options.method==='GET'?config:{message:'已保存'};
    let ok = true;
    if(url.endsWith('/backup'))data={format:'btclone-config-v1',role,config:{...config,wifiPass:'test-private-password'}};
    if(failSave && url.endsWith('/config') && options.method==='POST') {data={error:'参数范围不合法'};ok=false;}
    if(ok && url.endsWith('/config') && options.method==='POST') {
      const submitted=JSON.parse(options.body);queuedConfig={...config,...submitted,revision:config.revision+1};
      for(const key of ['expectedRevision','wifiPass','mqttPass','apPass','adminPass','clearWifiPass','clearMqttPass','csrfToken'])delete queuedConfig[key];
      data={message:'等待回读',revision:queuedConfig.revision,config:queuedConfig};
    }
    if(url.endsWith('/restore')) {
      const submitted=JSON.parse(options.body);queuedConfig={...config,...submitted.config,revision:config.revision+1};
      for(const key of ['csrfToken','wifiPass','mqttPass','apPass','adminPass'])delete queuedConfig[key];
      data={revision:queuedConfig.revision,config:queuedConfig};
    }
    return {ok,json:async()=>structuredClone(data)};
  };
  dom.window.eval(script);
  await new Promise(resolve=>setImmediate(resolve));
  const document = dom.window.document, form = document.querySelector('form');
  check(document.querySelector('[name=targetMac]').value===config.targetMac,'load MAC');
  check(document.querySelector('#backend-code').textContent.includes('11223344'),'show rolling code');
  check(!document.querySelector('img'),'status/log strings cannot inject HTML');
  dom.window.confirm=()=>false;const beforeExport=requests.length;await document.querySelector('#backup-config').onclick();
  check(requests.length===beforeExport&&!downloaded,'cancel credential export is a no-op');
  dom.window.confirm=()=>true;await document.querySelector('#backup-config').onclick();
  check(downloaded&&requests.at(-1).url.endsWith('/backup'),'explicit backup downloads a JSON file');
  check(requests.at(-1).options.headers['X-CSRF-Token']==='test-nonce','sensitive GET export carries CSRF');
  check(document.querySelector('.master').classList.contains('hidden')===(role!=='master'),'master controls scoped');
  check(document.querySelector('.slave').classList.contains('hidden')===(role!=='slave'),'slave controls scoped');
  const tabButtons=Array.from(document.querySelectorAll('[data-tab]'));
  check(tabButtons.map(b=>b.textContent).join('/')===(role==='master'?'概览/网络/蓝牙/计划/设备':'概览/网络/模拟/设备'),'approved role-specific navigation');
  check(document.querySelector('#panel-overview').hidden===false,'overview default page');
  check(document.querySelector('#state').closest('details').open===false,'JSON diagnostics collapsed by default');
  check(document.querySelectorAll('.stat').length===2,'connection status shown in cards');
  document.querySelector('[data-tab=network]').click();
  check(!document.querySelector('#panel-network').hidden&&document.querySelector('#panel-overview').hidden,'network tab switches panel');
  document.querySelector('[name=wifiSsid]').value='edited WiFi';
  document.querySelector('[name=wifiSsid]').dispatchEvent(new dom.window.Event('input',{bubbles:true}));
  document.querySelector('[data-tab=device]').click();document.querySelector('[data-tab=network]').click();
  check(document.querySelector('[name=wifiSsid]').value==='edited WiFi','tab navigation keeps unsaved values');
  const modeButtons=Array.from(document.querySelectorAll('[data-mode]'));
  modeButtons.find(b=>b.dataset.mode==='true').click();
  check(document.querySelector('[name=fixed]').value==='true','fixed-mode selection updates actual config field');
  check(document.querySelector('.fixed-fields').hidden===(role!=='slave'),'fixed fields shown only on actual slave');
  if(role==='master')modeButtons.find(b=>b.dataset.mode==='false').click();
  check(!Array.from(document.querySelectorAll('[data-action]')).some(b=>/模拟|演示/.test(b.textContent)),'no preview-only actions');
  check(document.querySelector('#save-state').textContent.includes('未保存'),'dirty editing status visible');
  const beforeDiscard=requests.length;dom.window.confirm=()=>false;
  await document.querySelector('[data-discard]').onclick();
  check(requests.length===beforeDiscard,'cancel discarding edits is no-op');dom.window.confirm=()=>true;
  const beforeWifiTest=requests.length;
  await document.querySelector('[data-action=wifi-test]').onclick();
  check(requests.length===beforeWifiTest,'unsaved WiFi test does not test obsolete credentials');
  check(document.querySelector('#save-feedback').textContent.includes('先保存'),'WiFi prerequisite visible by save button');
  document.querySelector('[name=scanSeconds]').value='25';
  document.querySelector('[name=wifiPass]').value='new-password';
  document.querySelector('[name=simulate]').checked=false;
  document.querySelector('[name=displayLock]').checked=true;
  document.querySelector('[name=retryAttempts]').value='4';
  if(role==='slave')document.querySelector('[name=fixed]').value='true';
  await form.onsubmit({preventDefault(){}});
  let request = requests.at(-1), body = JSON.parse(request.options.body);
  check(request.options.headers['X-CSRF-Token']==='test-nonce','mutation carries CSRF token');
  check(body.expectedRevision===1,'local writes carry expected configuration revision');
  check(body.slots.length===2&&body.slots[0].start===480,'local work slots roundtrip');
  check(body.maintenanceBootSeconds===120,'manual reset grace is preserved in form save');
  check(body.scanSeconds===25 && typeof body.scanSeconds==='number','numeric fields remain numbers');
  check(body.simulate===false && typeof body.tls==='boolean','checkbox fields remain booleans');
  check(body.fixed===(role==='slave'),'mode serialized as boolean');
  check(body.displayLock===true && body.recoveryRestart===false,'lock/restart options serialized');
  check(body.retryAttempts===4 && !('cooldownSeconds' in body) && body.connectSeconds===30,'recovery settings roundtrip without removed batch cooldown');
  check(document.body.textContent.includes('长按3秒没有反馈'),'silent locked hold documented');
  check(body.wifiPass==='new-password' && !('adminPass' in body) && !('mqttPass' in body),'only changed passwords submitted');
  check(!('wifiPasswordSet' in body) && !('mqttPasswordSet' in body),'read-only metadata excluded');
  check(document.querySelector('#binding-summary').textContent.includes('代次1'),'binding summary shown');
  check(document.body.textContent.includes('界面访问锁（非数据加密'),'access-lock wording');
  check(document.querySelector('#save-state').textContent.includes('等待'),'save not marked successful before reboot readback');
  const beforeDuplicate=requests.length;await form.onsubmit({preventDefault(){}});
  check(requests.length===beforeDuplicate,'pending reboot blocks duplicate submission');
  const committed=structuredClone(queuedConfig);
  Object.assign(config,queuedConfig,{broker:'wrong.example.com',csrfToken:'nonce-after-reboot'});status.configRevision=config.revision;
  await dom.window.refresh();
  check(document.querySelector('#save-feedback').textContent.includes('不一致'),'mismatched reboot readback does not claim success');
  Object.assign(config,committed);await dom.window.refresh();
  check(document.querySelector('#save-feedback').textContent.includes('回读确认'),'successful reboot readback verified');
  check(document.querySelector('[name=broker]').value==='mqtt.example.com' && document.querySelector('[name=mqttUser]').value==='test','MQTT host and user retained after reboot');
  check(document.querySelector('[name=wifiPass]').value==='','password still not returned');
  const beforeCancel=requests.length;
  document.querySelector('[name=masterId]').value='other-master';
  dom.window.confirm=()=>false;
  await form.onsubmit({preventDefault(){}});
  check(requests.length===beforeCancel,'cancel binding change sends no request');
  dom.window.confirm=()=>true;
  await form.onsubmit({preventDefault(){}});
  check(JSON.parse(requests.at(-1).options.body).masterId==='other-master','confirmed binding change submitted');
  check(requests.at(-1).options.headers['X-CSRF-Token']==='nonce-after-reboot','reboot renews token for next save');
  Object.assign(config,queuedConfig);status.configRevision=config.revision;await dom.window.refresh();
  failSave=true;
  await form.onsubmit({preventDefault(){}});
  check(document.querySelector('#feedback').textContent==='参数范围不合法','server validation error visible');
  check(document.querySelector('#save-feedback').textContent==='参数范围不合法','validation failure also visible beside save');
  config.broker='external.example.com';config.revision++;status.configRevision=config.revision;
  await dom.window.refresh();
  check(document.querySelector('[name=broker]').value==='external.example.com','idle page refreshes external config revisions');
  document.querySelector('[name=broker]').value='unsaved.example.com';form.oninput();
  config.revision++;status.configRevision=config.revision;await dom.window.refresh();
  check(document.querySelector('[name=broker]').value==='unsaved.example.com','background refresh preserves unsaved edits');
  const button = document.querySelector('[data-action=restart]');
  await button.onclick();
  request=requests.filter(r=>r.url.endsWith('/action')).at(-1);
  check(JSON.parse(request.options.body).action==='restart','manual command submitted');
  check(request.options.headers['X-CSRF-Token']==='nonce-after-reboot','manual command carries CSRF token');
  const endButton=document.querySelector('[data-action="maintenance-end"]');
  if(role==='master') {
    dom.window.confirm=()=>false;const beforeEnd=requests.length;await endButton.onclick();check(requests.length===beforeEnd,'cancel ending maintenance leaves connection intact');
    dom.window.confirm=()=>true;await endButton.onclick();check(JSON.parse(requests.filter(r=>r.url.endsWith('/action')).at(-1).options.body).action==='maintenance-end','explicit end-maintenance action uses real API');
  } else check(endButton.closest('.master').classList.contains('hidden'),'slave has no maintenance sleep window action');
  const input=document.querySelector('#backup-file');let backupInput;
  Object.defineProperty(input,'files',{get:()=>backupInput?[{size:500,text:async()=>JSON.stringify(backupInput)}]:[]});
  const beforeImport=requests.length;backupInput=null;await input.onchange({target:input});check(requests.length===beforeImport,'cancel file selection is a no-op');
  backupInput={format:'btclone-config-v1',role:role==='master'?'slave':'master',config:{}};await input.onchange({target:input});
  check(requests.length===beforeImport,'wrong-role backup never reaches firmware');
  backupInput={format:'btclone-config-v1',role,config:{...config,scanSeconds:35}};dom.window.confirm=()=>false;await input.onchange({target:input});
  check(requests.length===beforeImport,'cancel replacing configuration is a no-op');
  dom.window.confirm=()=>true;await document.querySelector('[data-discard]').onclick();await input.onchange({target:input});
  const restored=JSON.parse(requests.at(-1).options.body);
  check(requests.at(-1).url.endsWith('/restore')&&restored.expectedRevision===config.revision,'restore uses current revision and dedicated authenticated API');
  dom.window.close();
}
(async()=>{await test('master');await test('slave');console.log(`PASS ${checks} browser DOM/configuration flow checks (HTTP mocked)`);})().catch(error=>{console.error(error);process.exitCode=1;});
