"""Render firmware screen_ui.cpp with real U8g2 fonts into documentation assets."""
from pathlib import Path
import subprocess, re, os
from PIL import Image, ImageDraw, ImageFont
root=Path(__file__).resolve().parents[1]
out=root.parent/'.dev-evidence/screen-native'
out.mkdir(exist_ok=True)
clib=root/'.pio/libdeps/master_ml307/U8g2/src/clib'
compiler=Path('D:/WorkBuddyEnv/tools/usb-native/w64devkit/bin/g++.exe')
font_names=['5x7_tr','9x15B_tr','wqy12_t_gb2312','logisoso24_tr']
fonts=(clib/'u8g2_fonts.c').read_text(encoding='latin-1')
chosen=['#include "u8g2.h"']
for name in font_names:
    match=re.search(r'const uint8_t u8g2_font_'+name+r'\[.*?;\n',fonts,re.S)
    assert match,name
    chosen.append(match.group(0))
(out/'fonts.c').write_text('\n'.join(chosen),encoding='utf-8')
sources=['u8g2_setup','u8g2_ll_hvline','u8g2_hvline','u8g2_line','u8g2_box','u8g2_circle','u8g2_font','u8g2_intersection','u8x8_8x8','u8x8_string']
cmd=[str(compiler),'-std=c++17','-O1','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-static-libstdc++','-static-libgcc',
     '-I'+str(root/'tests/screen_host'),'-I'+str(root/'include'),'-I'+str(clib),str(root/'src/screen_ui.cpp'),
     str(root/'tests/screen_host/render.cpp'),str(out/'fonts.c')]
cmd += [str(clib/(s+'.c')) for s in sources]
exe=out/'render.exe'
subprocess.run(cmd+['-o',str(exe)],check=True)
env=os.environ.copy();env['TZ']='CST-8'
result=subprocess.run([str(exe),str(out)],check=True,capture_output=True,text=True,env=env)
print(result.stdout,end='')
(root.parent/'.dev-evidence/screen-test.log').write_text(result.stdout,encoding='utf-8')
assets=root/'docs/assets';assets.mkdir(exist_ok=True)
for pbm in out.glob('*.pbm'):
    image=Image.open(pbm).convert('L')
    # PBM convention is 1=black; OLED convention is 1=white.
    from PIL import ImageOps
    image=ImageOps.invert(image)
    image.save(assets/(pbm.stem+'.png'))
font=ImageFont.truetype('C:/Windows/Fonts/msyh.ttc',16)
sheet=Image.new('RGB',(640,744),'#10151d');draw=ImageDraw.Draw(sheet)
labels=['时钟首页','六宫格总览','当前滚动码','蓝牙采集','MQTT上传','数据记录','工作计划','设备维护']
for i in range(8):
    x=24+(i%2)*316;y=16+(i//2)*182
    draw.text((x,y),labels[i],font=font,fill='#cbd6e4')
    sheet.paste(Image.open(assets/f'master-{i}.png').resize((256,128),Image.Resampling.NEAREST),(x,y+30))
sheet.save(assets/'firmware-pages.png')
