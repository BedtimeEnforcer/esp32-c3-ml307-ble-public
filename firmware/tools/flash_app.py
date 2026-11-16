"""APP-only update for the two measured boards; preserve configuration before boot.

Does not apply private credentials. After flashing, authenticate through USB and
verify/apply the local maintenance policy separately, then test the real device.
"""
from pathlib import Path
import argparse, datetime, hashlib, json, re, shutil, struct, subprocess, sys

ROOT=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--port',required=True)
parser.add_argument('--role',choices=['master','slave'],required=True)
parser.add_argument('--version',default='0.3.7-dev')
parser.add_argument('--extended-backup',type=Path,help='Reuse a 640KB master NVS backup only after comparing it with the live chip')
args=parser.parse_args()
if not re.fullmatch(r'COM\d+',args.port,re.I) or not re.fullmatch(r'0\.\d+\.\d+-dev',args.version):
    raise ValueError('Invalid serial port or version')
folder=ROOT/('dist/v'+args.version)
label='master-ML307' if args.role=='master' else 'slave-C3'
image=folder/(label+'-v'+args.version+'-app.bin')
manifest=json.loads((folder/'manifest.json').read_text(encoding='utf-8'))
expected=manifest['artifacts'][image.name]
digest=hashlib.sha256(image.read_bytes()).hexdigest()
assert digest==expected['sha256'] and image.stat().st_size==expected['bytes']
assert 1000000<image.stat().st_size<=(0x1A0000 if args.role=='master' else 0x300000)
stamp=datetime.datetime.now().astimezone().strftime('%Y%m%d-%H%M%S')
out=ROOT/'private/device-flash'/(stamp+'-'+args.port+'-'+args.role+'-v'+args.version)
out.mkdir(parents=True,exist_ok=False)
tool=Path.home()/'.platformio/packages/tool-esptoolpy/esptool.py'
def run(name,command,read=False,reset=False):
    exe=ROOT/'tools/esptool_safe_read.py' if read else tool
    with (out/(name+'.private.log')).open('wb') as log:
        result=subprocess.run([sys.executable,str(exe),'--chip','esp32c3','--port',args.port,'--baud','115200','--after','hard_reset' if reset else 'no_reset',*command],stdout=log,stderr=subprocess.STDOUT,timeout=90)
    if result.returncode:raise RuntimeError(name+' failed; private log retained')
    print(name+': OK',flush=True)
    return (out/(name+'.private.log')).read_text(encoding='utf-8',errors='replace')
wrote=False;ready=False
try:
    identity=run('01-identify',['flash_id'])
    assert 'Detected flash size: 4MB' in identity
    before=out/'before-0x0-0x10000.private.bin'
    run('02-backup',['read_flash','0','0x10000',str(before)],read=True)
    original=before.read_bytes();assert len(original)==65536
    partitions=[]
    for pos in range(0x8000,0x8C00,32):
        block=original[pos:pos+32]
        if block[:2]!=b'\xaa\x50':continue
        _,typ,sub,offset,size,label,flags=struct.unpack('<HBBII16sI',block)
        partitions.append((typ,sub,offset,size))
    size=0x1A0000 if args.role=='master' else 0x300000
    assert (0,0x10,0x10000,size) in partitions,'Unexpected APP layout; no write performed'
    assert (1,2,0x9000,0x5000) in partitions,'Unexpected NVS layout; no write performed'
    if args.role=='master':
        # COM8's measured boot slot is app0. Do not overwrite an inactive slot
        # and then silently report a successful running upgrade.
        assert int.from_bytes(original[0xe000:0xe004],'little')==1,'OTA selection changed; inspect before flashing'
        assert (1,2,0x350000,0xA0000) in partitions,'Unexpected extended NVS layout; no write performed'
        extended=out/'before-expanded-nvs.private.bin'
        if args.extended_backup:
            assert args.extended_backup.stat().st_size==0xA0000
            run('02b-confirm-existing-expanded-backup',['verify_flash','0x350000',str(args.extended_backup)])
            shutil.copyfile(args.extended_backup,extended)
        else:run('02b-backup-expanded-nvs',['read_flash','0x350000','0xA0000',str(extended)],read=True)
    wrote=True
    run('03-write-app',['write_flash','--flash_mode','dio','--flash_freq','40m','--flash_size','4MB','0x10000',str(image)])
    run('04-verify-app',['verify_flash','0x10000',str(image)])
    after=out/'after-0x0-0x10000.private.bin'
    exactReadback=True
    try:run('05-preserve-check',['read_flash','0','0x10000',str(after)],read=True)
    except (RuntimeError,subprocess.TimeoutExpired):
        # The saved prewrite backup is already complete and transport-verified.
        # Compare it with a fresh chip-side digest; never rewrite the APP to
        # recover from a postwrite USB read error, and never waive this check.
        exactReadback=False
        run('05a-preserved-chip-digest',['verify_flash','0',str(before)])
    if exactReadback:assert original==after.read_bytes(),'Preserved region changed; left in loader for inspection'
    if args.role=='master':run('05b-preserve-expanded-nvs',['verify_flash','0x350000',str(extended)])
    ready=True
    report={'port':args.port,'role':args.role,'version':args.version,'imageSha256':digest,'appVerified':True,'preservedRegionEqualBeforeBoot':True,'preservedRegionVerification':'byte_readback' if exactReadback else 'chip_md5_against_verified_prewrite_backup','expandedNvsPreservedBeforeBoot':args.role=='master','hardwareAccepted':False,'privateEvidence':str(out),'maintenancePolicyReadbackPending':True}
    (out/'verification.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
finally:
    if not wrote or ready:run('06-restart',['flash_id'],reset=True)
print(json.dumps(report,ensure_ascii=False),flush=True)
