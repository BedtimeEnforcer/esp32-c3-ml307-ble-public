"""Build a self-contained manual from the current guide and measured hardware record."""
from pathlib import Path
import html,re,json
root=Path(__file__).resolve().parents[1]
import argparse
parser=argparse.ArgumentParser();parser.add_argument('--kind',choices=['usage','flash'],default='usage');args=parser.parse_args()
if args.kind=='flash':
    md=(root/'docs/v0.3.7-固件烧录说明.md').read_text(encoding='utf-8')
else:
    md=(root/'docs/v0.3.7-使用与维护.md').read_text(encoding='utf-8')

def inline(text):
    text=html.escape(text)
    text=re.sub(r'`([^`]+)`',r'<code>\1</code>',text)
    return re.sub(r'https?://[A-Za-z0-9._~:/?#\[\]@!$&()*+,;=%-]+',lambda m:'<a href="'+m[0]+'">'+m[0]+'</a>',text)
parts=[];toc=[];lines=md.splitlines();i=0
while i<len(lines):
    line=lines[i];i+=1
    if not line.strip():continue
    if line.startswith('## '):
        sid='section-'+str(len(toc)+1);toc.append((sid,line[3:]));parts.append('<h2 id="'+sid+'">'+inline(line[3:])+'</h2>');continue
    if line.startswith('# '):parts.append('<h1>'+inline(line[2:])+'</h1>');continue
    if line.startswith('|'):
        rows=[line]
        while i<len(lines) and lines[i].startswith('|'):rows.append(lines[i]);i+=1
        parts.append('<div class="table"><table>')
        for n,row in enumerate(rows):
            cells=[x.strip() for x in row.strip('|').split('|')]
            if all(re.fullmatch(r'[-: ]+',x) for x in cells):continue
            tag='th' if n==0 else 'td';parts.append('<tr>'+''.join('<'+tag+'>'+inline(x)+'</'+tag+'>' for x in cells)+'</tr>')
        parts.append('</table></div>');continue
    if line.startswith('- ') or re.match(r'^\d+\. ',line):
        parts.append('<p class="item">'+inline(line)+'</p>');continue
    parts.append('<p>'+inline(line)+'</p>')
nav=''.join('<a href="#'+sid+'">'+inline(name)+'</a>' for sid,name in toc)
style='''*{box-sizing:border-box}html{scroll-behavior:smooth}body{margin:0;background:#f2f5f5;color:#233c43;font:15px/1.85 system-ui,"Microsoft YaHei",sans-serif}header{padding:38px max(22px,calc((100% - 1160px)/2));background:#173f46;color:white}header p{max-width:850px;color:#cfe1e0}header small{letter-spacing:.15em;color:#d5e5ad}.layout{max-width:1200px;margin:auto;display:grid;grid-template-columns:235px minmax(0,1fr);gap:30px;padding:30px 22px}nav{align-self:start;position:sticky;top:16px;max-height:95vh;overflow:auto}nav a{display:block;color:#41676d;text-decoration:none;font-size:13px;padding:7px 10px;border-left:2px solid #cadbda}article{min-width:0;background:white;border-radius:16px;padding:28px 34px}h1{font-size:27px;line-height:1.4}h2{font-size:20px;margin-top:35px;padding-top:20px;border-top:1px solid #e5eceb;scroll-margin-top:20px}p{overflow-wrap:anywhere}.item{padding-left:14px;border-left:3px solid #dce8d7}.table{overflow-x:auto}table{width:100%;border-collapse:collapse;font-size:13px}td,th{border-bottom:1px solid #dce7e5;text-align:left;vertical-align:top;padding:12px;overflow-wrap:anywhere}th{background:#eff4f1}a{color:#1c7278;overflow-wrap:anywhere}code{background:#edf2ef;padding:2px 5px;border-radius:4px}footer{max-width:1160px;margin:20px auto;padding:20px;color:#637d80;font-size:12px}@media(max-width:760px){.layout{display:block;padding:16px}nav{position:static;display:flex;overflow:auto;margin-bottom:15px;gap:6px;max-height:none}nav a{white-space:nowrap;border:1px solid #d2dfdb;border-radius:9px}article{padding:20px}header{padding:28px 22px}h1{font-size:23px}}@media print{nav{display:none}.layout{display:block}body{background:white}header{padding:20px;color:#173f46;background:white}header p{color:#41676d}article{padding:0}a{color:inherit}}'''
output='<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>蓝牙同步设备 v0.3.7 使用与实机验收手册</title><style>'+style+'</style><header><small>DEVICE GUIDE · DEMO / DMEO</small><h1>蓝牙同步设备 · v0.3.7</h1><p>日常操作与RESET维护窗口；烧录说明另册。本版仅编译和软件验证，未烧录或实机验收。</p></header><div class="layout"><nav>'+nav+'</nav><article>'+''.join(parts)+'</article></div><footer>离线文档，无外部脚本或字体。本文不包含设备专用密码、SIM 身份、平台 API 或真实原始广播。</footer></html>'
if args.kind=='flash':output=output.replace('使用与实机验收手册','固件烧录手册').replace('蓝牙同步设备 · v0.3.7</h1>','蓝牙同步设备 · 固件烧录</h1>')
path=root/('docs/蓝牙同步设备-v0.3.7-'+('固件烧录' if args.kind=='flash' else '使用')+'手册.html');path.write_text(output,encoding='utf-8');print(json.dumps({'kind':args.kind,'chapters':len(toc),'bytes':path.stat().st_size}))
