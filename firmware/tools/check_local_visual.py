from pathlib import Path
import subprocess,json,re,html,concurrent.futures
root=Path(__file__).resolve().parents[2]
keys=['slave-overview','slave-network','slave-radio','slave-radio-fixed','slave-device','master-overview','master-schedule']
def capture(key):
    output=root/'.dev-evidence'/('local-'+key+'.html')
    args=[r'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe','--headless','--disable-gpu','--no-first-run','--user-data-dir='+str(root/'.dev-evidence'/('edge-visual-final-'+key)),'--window-size=920,1800','--virtual-time-budget=5500','--screenshot='+str(root/'.dev-evidence'/('local-'+key+'.png')),'--dump-dom','http://127.0.0.1:8767/compare-'+key+'.html']
    with output.open('w',encoding='utf-8') as out,(root/'.dev-evidence'/('edge-visual-final-'+key+'.log')).open('w',encoding='utf-8') as err:
        p=subprocess.run(args,stdout=out,stderr=err,timeout=35);assert p.returncode==0
    text=output.read_text(encoding='utf-8');m=re.search(r'<pre[^>]*id="comparison-qa"[^>]*>(.*?)</pre>',text,re.S);assert m,key+' missing browser result'
    data=json.loads(html.unescape(m[1]));assert data['fixture'] and data['width']==390 and not data['overflow'],(key,data)
    expected=['概览','网络','模拟','设备'] if key.startswith('slave') else ['概览','网络','蓝牙','计划','设备']
    assert data['nav']==expected and data['diagCollapsed'] and data['cards']==2 and data['palette']=='#147d68'
    tab=key.split('-')[1];assert data['active']==[tab]
    data['scenario']=key;return data
with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:results=list(pool.map(capture,keys))
report={'kind':'local_browser_visual_contract','approved_design':'local-maintenance-backend','source_check':json.loads((root/'.dev-evidence/local-ui-source-check.json').read_text()),'cases':results,'real_api':False,'real_device_acceptance':False,'production_demo_data':False}
(root/'.dev-evidence/local-ui-visual-check.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
print(json.dumps({'cases':len(results),'phone_viewport':390,'overflow':False,'approved_palette':True,'tabs_match':True,'real_device_acceptance':False}))
