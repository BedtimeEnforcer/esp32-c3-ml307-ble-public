"""Verify exported images against source hashes and local-only credentials."""
from pathlib import Path
import hashlib
import json
import shutil
import gzip
import re
import argparse

ROOT = Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--version',default='0.3.7-dev');args=parser.parse_args()
if not re.fullmatch(r'0\.\d+\.\d+-dev',args.version):raise ValueError('Invalid development version')
OUT = ROOT / ('dist/v'+args.version)
manifest = json.loads((OUT / 'manifest.json').read_text(encoding='utf-8'))
for relative,expected in manifest['monitor_hashes'].items():
    assert hashlib.sha256((ROOT.parent/'remote-web'/relative).read_bytes()).hexdigest()==expected,'Monitor/source mismatch: '+relative
for relative, expected in manifest['source_hashes'].items():
    assert hashlib.sha256((ROOT / relative).read_bytes()).hexdigest() == expected, 'Source no longer matches image: ' + relative
for relative,expected in manifest['monitor_hashes'].items():
    assert hashlib.sha256((ROOT.parent/'remote-web'/relative).read_bytes()).hexdigest()==expected,'Monitor source mismatch: '+relative
needles = []
private = ROOT / 'private'
if (private / 'emqx-platform.local.json').exists():
    credentials = json.loads((private / 'emqx-platform.local.json').read_text(encoding='utf-8-sig'))
    needles.extend(credentials.get(k, '').encode() for k in ('key', 'secret'))
if (private / 'device-provisioning.local.json').exists():
    devices = json.loads((private / 'device-provisioning.local.json').read_text(encoding='utf-8'))
    needles.extend(devices[role]['password'].encode() for role in ('master', 'slave'))
    needles.append(devices['targetMac'].encode())
if (private/'device-maintenance-policy.local.json').exists():
    policy=json.loads((private/'device-maintenance-policy.local.json').read_text(encoding='utf-8'))
    needles.extend(policy[k].encode() for k in ('apPass','adminPass'))
needles = [n for n in needles if len(n) >= 8]
if (private/'remote-monitor.local.json').exists():
    remote=json.loads((private/'remote-monitor.local.json').read_text(encoding='utf-8'))
    needles.append(remote['password'].encode())
for filename, expected in manifest['artifacts'].items():
    content = (OUT / filename).read_bytes()
    assert len(content) == expected['bytes'] and hashlib.sha256(content).hexdigest() == expected['sha256'], 'Image hash mismatch: ' + filename
    assert not any(n in content for n in needles), 'Private material unexpectedly embedded: ' + filename
    page=(ROOT/'web/index.html').read_text(encoding='utf-8').encode('utf-8')
    assert hashlib.sha256(page).hexdigest()==manifest['embedded_web_sha256'] and page in content,'Actual compiled image does not contain current local web page: '+filename
    header=(ROOT/'include/web_page.h').read_text(encoding='utf-8')
    compressed=bytes(map(int,re.search(r'WEB_PAGE_GZIP\[\].*?\{([^}]+)\}',header,re.S)[1].split(',')))
    assert gzip.decompress(compressed)==page and compressed in content,'Compressed web asset does not match canonical page: '+filename
evidence = OUT / 'evidence'
evidence.mkdir(exist_ok=True)
for filename in ('core-test.log','runtime-test.log','web-test.log','remote-web-test.log','manual-v037-check.json','oled-init-static-check.json','local-ui-v037-visual-check.json','remote-browser-check-v037.json'):
    source = ROOT.parent / '.dev-evidence' / filename
    if source.exists():
        shutil.copy2(source, evidence / filename)
report = {'image_hashes_verified': True, 'source_hashes_verified': True,
          'private_credential_literals_absent_from_images': bool(needles),
          'hardware_accepted': False,'new_cloud_permissions_verified':manifest.get('new_cloud_permissions_verified',False),'monitor_hashes_verified':True,'current_web_page_found_in_all_images':True, 'image_count': len(manifest['artifacts'])}
(OUT / 'verification.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
print(json.dumps(report, indent=2))
