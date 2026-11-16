"""Authenticated USB diagnostics/configuration; private inputs never enter images."""
from pathlib import Path
import argparse
import json
import time
import serial
import sys

if hasattr(sys.stdout,'reconfigure'):sys.stdout.reconfigure(encoding='utf-8')

ROOT = Path(__file__).resolve().parents[1]

def checksum(payload):
    value=2166136261
    for byte in payload:value=((value^byte)*16777619)&0xffffffff
    return value


def exchange(port_name, password, op, values=None, revision=None):
    request = {'op': op, 'password': password}
    if values is not None:
        request.update(values=values, expectedRevision=revision)
    encoded=json.dumps(request,ensure_ascii=True).encode()
    if len(encoded) > 9500:
        raise ValueError('USB request exceeds 9500 JSON bytes')
    payload=('BTLOCAL2 '+str(len(encoded))+' '+str(checksum(encoded))+' ').encode()+encoded+b'\n'
    port = serial.Serial(port=None, baudrate=115200, timeout=.1, write_timeout=2)
    port.port = port_name
    port.rts = False
    port.dtr = True
    port.open()
    try:
        time.sleep(.2)
        port.reset_input_buffer()
        # Leave headroom in the native USB RX buffer while the business loop runs.
        for start in range(0, len(payload), 32):
            port.write(payload[start:start + 32])
            time.sleep(.02)
        deadline = time.monotonic() + 12
        line = bytearray()
        while time.monotonic() < deadline:
            char = port.read(1)
            if char == b'\n':
                raw = bytes(line).strip()
                line.clear()
                if raw.startswith(b'BTLOCAL2 '):
                    _,size,crc,body=raw.split(b' ',3)
                    if len(body)!=int(size) or checksum(body)!=int(crc):
                        raise ValueError('USB reply integrity failed; read back before retrying any write')
                    return json.loads(body)
                if raw.startswith(b'BTLOCAL ') and op in ('diagnostic','backup'):
                    return json.loads(raw[8:])
            elif char:
                line += char
                if len(line) > 12000:
                    line.clear()
        raise TimeoutError('USB acknowledgement not received; read back before retrying a write')
    finally:
        port.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', default='COM6')
    parser.add_argument('--auth-file', type=Path, help='Private JSON with password; otherwise first-setup default')
    parser.add_argument('--patch-file', type=Path, help='Private JSON object with configuration changes')
    parser.add_argument('--backup-file', type=Path, help='Explicit export including credentials; save outside Git')
    parser.add_argument('--restore-file', type=Path, help='Restore an authenticated export for the same role')
    parser.add_argument('--apply-policy', action='store_true')
    parser.add_argument('--open-ap', action='store_true', help='Authenticated; rejected while display access is locked')
    parser.add_argument('--policy-file', type=Path, default=ROOT / 'private/device-maintenance-policy.local.json')
    args = parser.parse_args()
    password = json.loads(args.auth_file.read_text(encoding='utf-8'))['password'] if args.auth_file else '88888888'
    before = exchange(args.port, password, 'diagnostic')
    if not before.get('ok'):
        raise RuntimeError('USB authentication/diagnostic rejected: ' + before.get('error', 'unknown'))
    if args.backup_file:
        if args.backup_file.exists():raise FileExistsError('Choose a new backup path; existing backups are preserved')
        if args.patch_file or args.restore_file or args.apply_policy or args.open_ap:raise ValueError('Export must be a separate read-only operation')
        exported=exchange(args.port,password,'backup')
        if not exported.get('ok') or exported.get('format')!='btclone-config-v1':raise RuntimeError('Configuration export rejected')
        args.backup_file.parent.mkdir(parents=True,exist_ok=True)
        with args.backup_file.open('x',encoding='utf-8') as stream:json.dump(exported,stream,ensure_ascii=False,indent=2)
        print(json.dumps({'backup_saved':str(args.backup_file),'contains_credentials':True}));return
    if args.open_ap:
        result=exchange(args.port,password,'maintenance')
        if not result.get('ok'):raise RuntimeError('Maintenance AP rejected: '+result.get('error','unknown'))
        print(json.dumps({'maintenance_ap_open':True}));return
    if not args.patch_file and not args.apply_policy and not args.restore_file:
        print(json.dumps(before, ensure_ascii=False, indent=2))
        return
    values = json.loads(args.patch_file.read_text(encoding='utf-8')) if args.patch_file else {}
    if args.apply_policy:
        policy = json.loads(args.policy_file.read_text(encoding='utf-8'))
        values.update({key: policy[key] for key in ('apPass', 'adminPass')})
    op='config'
    if args.restore_file:
        if args.patch_file or args.apply_policy:raise ValueError('Restore and patch are separate operations')
        values=json.loads(args.restore_file.read_text(encoding='utf-8-sig'));op='restore'
        if values.get('format')!='btclone-config-v1' or not isinstance(values.get('config'),dict):raise ValueError('Invalid backup format')
    result = exchange(args.port, password, op, values, before['revision'])
    if not result.get('ok'):
        raise RuntimeError('USB configuration rejected: ' + result.get('error', 'unknown'))
    print(json.dumps({'write_verified': True, 'revision': result['revision'], 'restart_readback_pending': True}))
    # Acknowledgement means NVS readback passed. Boot-time loading is a separate check.
    time.sleep(4)
    restored=values['config'] if op=='restore' else values
    after = exchange(args.port, restored.get('adminPass') or password, 'diagnostic')
    if not after.get('ok') or after['revision'] != result['revision']:
        raise RuntimeError('Configuration written but reboot readback not confirmed')
    exported=exchange(args.port,restored.get('adminPass') or password,'backup')
    if not exported.get('ok'):raise RuntimeError('Restarted but configuration value readback failed')
    ignored={'revision','wifiGeneration','mqttGeneration','appliedCommand','csrfToken','expectedRevision'}
    for key,value in restored.items():
        if key in ignored or key.startswith('clear') or key not in exported['config']:continue
        if op=='config' and key in ('wifiPass','mqttPass','apPass','adminPass') and not value:continue
        actual=exported['config'][key]
        if key in ('targetMac','fixedAdv','fixedRsp'):actual=str(actual).upper();value=str(value).upper()
        if actual!=value:raise RuntimeError('Restarted configuration differs in '+key+'; do not repeat the write blindly')
    after['submittedValuesVerifiedAfterReboot']=True
    print(json.dumps({'reboot_readback_verified': True, **after}, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
