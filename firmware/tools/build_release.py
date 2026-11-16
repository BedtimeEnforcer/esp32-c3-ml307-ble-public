"""Rebuild both roles and export separate development images with SHA256."""
from pathlib import Path
import hashlib
import json
import shutil
import subprocess
import sys
import os
import argparse

ROOT = Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--version',default='0.3.7-dev')
args=parser.parse_args()
if args.version!='0.3.7-dev':raise ValueError('This builder exports only the current v0.3.7-dev; old releases are preserved.')
OUT = ROOT / ('dist/v'+args.version)
OUT.mkdir(parents=True, exist_ok=True)
subprocess.run([sys.executable, str(ROOT / 'tools/embed_web.py')], check=True)
def source_hashes():
    values = {}
    for directory in ('include', 'src', 'web'):
        for p in sorted((ROOT / directory).rglob('*')):
            if p.is_file():
                values[p.relative_to(ROOT).as_posix()] = hashlib.sha256(p.read_bytes()).hexdigest()
    values['platformio.ini'] = hashlib.sha256((ROOT / 'platformio.ini').read_bytes()).hexdigest()
    values['tools/build_paths.py'] = hashlib.sha256((ROOT/'tools/build_paths.py').read_bytes()).hexdigest()
    return values

sources = source_hashes()
monitor=ROOT.parent/'remote-web'
public_files=json.loads((monitor/'publish-files.json').read_text(encoding='utf-8'))['files']
monitor_hashes={name:hashlib.sha256((monitor/name).read_bytes()).hexdigest() for name in public_files}
build_base=Path(os.environ.get('PLATFORMIO_BUILD_DIR',str(Path.home()/'.platformio/btclone-v030-build') if os.name=='nt' else str(ROOT/'.pio/build')))
build_env=os.environ.copy();build_env['PLATFORMIO_BUILD_DIR']=str(build_base)
log = OUT / 'build.log'
with log.open('w', encoding='utf-8') as output:
    result = subprocess.run([sys.executable, '-m', 'platformio', 'run', '-e', 'master_ml307', '-e', 'slave_c3'],
                            cwd=ROOT, stdout=output, stderr=subprocess.STDOUT,env=build_env)
if result.returncode:
    print(log.read_text(encoding='utf-8')[-6000:])
    raise SystemExit(result.returncode)
if source_hashes() != sources:
    raise RuntimeError('Source changed during compilation; rerun before exporting images.')
if monitor_hashes!={name:hashlib.sha256((monitor/name).read_bytes()).hexdigest() for name in public_files}:
    raise RuntimeError('Monitor source changed during compilation; rerun for a consistent delivery.')
pio = Path.home() / '.platformio/packages'
esptool = pio / 'tool-esptoolpy/esptool.py'
boot_app = pio / 'framework-arduinoespressif32/tools/partitions/boot_app0.bin'
artifacts = {}
for env, label in [('master_ml307', 'master-ML307'), ('slave_c3', 'slave-C3')]:
    build = build_base / env
    app = OUT / f'{label}-v{args.version}-app.bin'
    merged = OUT / f'{label}-v{args.version}-flash0.bin'
    shutil.copy2(build / 'firmware.bin', app)
    if env=='master_ml307' and app.stat().st_size>0x1A0000:
        raise RuntimeError('Master APP exceeds the measured COM8 factory app0 partition; do not flash it.')
    subprocess.run([sys.executable, str(esptool), '--chip', 'esp32c3', 'merge_bin', '--flash_mode', 'dio',
                    '--flash_freq', '40m', '--flash_size', '4MB', '-o', str(merged),
                    '0x0', str(build / 'bootloader.bin'), '0x8000', str(build / 'partitions.bin'),
                    '0xe000', str(boot_app), '0x10000', str(app)], check=True)
    for p in (app, merged):
        artifacts[p.name] = {'bytes': p.stat().st_size, 'sha256': hashlib.sha256(p.read_bytes()).hexdigest()}
canonical_page=(ROOT/'web/index.html').read_text(encoding='utf-8').encode('utf-8')
cloud_file=OUT/'evidence/remote-permissions.json'
cloud=json.loads(cloud_file.read_text(encoding='utf-8')) if cloud_file.exists() else {}
grants=cloud.get('subacks',{})
cloud_verified=bool(cloud.get('authenticated') and grants.get('raw')==1 and grants.get('master/device')==1 and grants.get('slave/device')==1 and grants.get('outsideBinding',0)>=128)
manifest = {'version': args.version, 'hardware_accepted': False,'new_cloud_permissions_verified':cloud_verified,
            'cloud_verification_scope':('current web account: grouped subscriptions allowed and outside binding rejected' if cloud_verified else 'not performed for this release; software and build checks only'),
            'embedded_web_sha256':hashlib.sha256(canonical_page).hexdigest(),'embedded_web_bytes':len(canonical_page),
            'oled_pin_initialization_fix_included':True,'build_platform':sys.platform,
            'master_factory_app0_limit_bytes':0x1A0000,
            'master_com8_upgrade':'APP only at 0x10000; preserve the measured factory OTA partition table and NVS; do not use flash0 on this board',
            'flash_assumption': '4MB, DIO 40MHz, verify actual flash before flashing',
            'source_hashes': sources,'monitor_hashes':monitor_hashes, 'artifacts': artifacts}
(OUT / 'manifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding='utf-8')
print(log.read_text(encoding='utf-8')[-1800:])
print('Exported development images; hardware acceptance is pending.')
