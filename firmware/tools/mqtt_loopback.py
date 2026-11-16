"""TLS/QoS1/retain/multiple subscriber/ACL test using synthetic bytes only.
Cloud mode uses the private per-device settings. It removes the synthetic
retained packet before exit. Never sends the real target's samples.
"""
import argparse
import json
from pathlib import Path
import queue
import secrets
import ssl
import subprocess
import threading
import time
import paho.mqtt.client as mqtt

ROOT = Path(__file__).resolve().parents[1]
EVIDENCE = ROOT.parent / '.dev-evidence'
OBSERVED = int(time.time()) - 7200


class Client:
    def __init__(self, host, port, name, credentials=None, ca=None):
        self.messages = queue.Queue()
        self.subacks = queue.Queue()
        self.connection = threading.Event()
        self.code = None
        self.client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=name, protocol=mqtt.MQTTv311)
        if credentials:
            self.client.username_pw_set(credentials['username'], credentials['password'])
        if ca:
            self.client.tls_set_context(ssl.create_default_context(cafile=str(ca)))
        self.client.on_connect = self.on_connect
        self.client.on_message = lambda c, u, m: self.messages.put(m)
        self.client.on_subscribe = lambda c, u, mid, codes, props: self.subacks.put(codes)
        self.client.connect(host, port, keepalive=30)
        self.client.loop_start()
        if not self.connection.wait(15):
            self.close()
            raise TimeoutError('MQTT CONNACK timeout')

    def on_connect(self, client, userdata, flags, code, properties):
        self.code = code
        self.connection.set()

    def subscribe(self, topic):
        self.client.subscribe(topic, qos=1)
        return self.subacks.get(timeout=10)

    def publish(self, topic, payload, retain=True):
        info = self.client.publish(topic, payload, qos=1, retain=retain)
        info.wait_for_publish(timeout=10)
        if not info.is_published():
            raise TimeoutError('QoS1 PUBACK timeout')

    def message(self, timeout=10):
        return self.messages.get(timeout=timeout)

    def close(self):
        self.client.disconnect()
        self.client.loop_stop()


def packet(seq=1, code='11223344'):
    return {'v': 1, 'device_id': 'master-01', 'generation': 1,
            'mac': 'C0:11:22:33:44:55', 'addr_type': 'random_static', 'adv_type': 'ADV_IND',
            'adv_hex': '0201061AFF4C00021500112233445566778899AABBCCDDEEFF' + code + 'C5',
            'scan_rsp_hex': '020A000303A5FD0E16A5FD64' + code + 'C0001122334408094D426561636F6E',
            'seq': seq, 'observed_at': OBSERVED, 'changed_at': OBSERVED,
            'reported_at': int(time.time()), 'time_valid': True, 'rssi': -65,
            'battery_est_pct': None, 'battery_estimated': True}


def run(cloud=False, port=18883, native=False, negative=False):
    EVIDENCE.mkdir(exist_ok=True)
    settings = json.loads((ROOT / 'private/device-provisioning.local.json').read_text(encoding='utf-8')) if cloud else None
    host = settings['broker'] if cloud else '127.0.0.1'
    topic = f"btclone/{settings['prefix']}/{settings['masterId']}/raw" if cloud else 'btclone/local-test/master-01/raw'
    port = 8883 if cloud else port
    ca = ROOT / 'private/emqxsl-ca.crt' if cloud else None
    credentials = settings['master'] if cloud else None
    slave = settings['slave'] if cloud else None
    suffix = secrets.token_hex(3)
    clients = []
    report = {'mode': 'private-cloud' if cloud else 'local', 'host': host, 'tls_verified': cloud, 'checks': []}

    def check(label, condition):
        if not condition:
            raise AssertionError(label)
        report['checks'].append(label)
        print('PASS', label, flush=True)

    def make(name, creds):
        c = Client(host, port, name + '-' + suffix, creds, ca)
        clients.append(c)
        return c

    publisher = None
    try:
        if cloud and negative:
            wrong = dict(credentials, password=secrets.token_hex(16))
            denied = make('wrong-password', wrong)
            check('wrong password rejected', denied.code.is_failure)
        publisher = make('master-test', credentials)
        check('master authenticated (' + str(publisher.code) + ')', not publisher.code.is_failure)
        first = packet()
        publisher.publish(topic, json.dumps(first))
        check('QoS1 PUBACK received', True)
        a, b = make('slave-test-a', slave), make('slave-test-b', slave)
        for label, client in [('A', a), ('B', b)]:
            check('subscriber ' + label + ' authenticated (' + str(client.code) + ')', not client.code.is_failure)
            codes = client.subscribe(topic)
            check('subscriber ' + label + ' SUBACK granted', not codes[0].is_failure)
            msg = client.message()
            check('late subscriber ' + label + ' receives retained original timestamp', msg.retain and json.loads(msg.payload) == first)
        second = packet(2, 'AABBCCDD')
        publisher.publish(topic, json.dumps(second))
        for label, client in [('A', a), ('B', b)]:
            msg = client.message()
            check('subscriber ' + label + ' receives changed raw', json.loads(msg.payload) == second)
            if native:
                exe = EVIDENCE / 'core_test.exe'
                result = subprocess.run([str(exe), 'decode'], input=msg.payload.decode(), text=True, capture_output=True, check=True)
                decoded = json.loads(result.stdout)
                check('firmware C++ decoder accepts subscriber ' + label, decoded['ok'] and decoded['rolling'] == 'AABBCCDD' and decoded['match'])
        a.close()
        again = make('slave-reconnected', slave)
        again.subscribe(topic)
        msg = again.message()
        check('reconnected subscriber receives latest retained seq and original observation time', msg.retain and json.loads(msg.payload)['seq'] == 2 and json.loads(msg.payload)['observed_at'] == second['observed_at'])
        if cloud:
            codes = b.subscribe(topic.replace('/master-01/', '/other-master/'))
            check('cross-master subscription rejected by ACL', codes[0].is_failure)
            codes = publisher.subscribe(topic)
            check('master subscription rejected by ACL', codes[0].is_failure)
            b.publish(topic, json.dumps(packet(999, 'DEADBEEF')), retain=False)
            try:
                unexpected = again.message(timeout=3)
                allowed = json.loads(unexpected.payload).get('seq') == 999
            except queue.Empty:
                allowed = False
            check('slave publish rejected by ACL (no forged packet delivered)', not allowed)
        report['passed'] = True
    except Exception as error:
        report['passed'] = False
        report['error'] = type(error).__name__ + ': ' + str(error)
        raise
    finally:
        if publisher and publisher.code and not publisher.code.is_failure:
            try:
                publisher.publish(topic, b'')
                report['synthetic_retained_packet_cleared'] = True
            except Exception:
                report['synthetic_retained_packet_cleared'] = False
        for c in clients:
            try:
                c.close()
            except Exception:
                pass
        filename = 'cloud-mqtt-test.json' if cloud else 'local-mqtt-test.json'
        (EVIDENCE / filename).write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--cloud', action='store_true')
    parser.add_argument('--port', type=int, default=18883)
    parser.add_argument('--native', action='store_true')
    parser.add_argument('--negative', action='store_true', help='Also check one incorrect password; avoid repeating this check')
    parser.add_argument('--auth-only', action='store_true')
    parser.add_argument('--acl-check', action='store_true')
    args = parser.parse_args()
    if args.acl_check:
        settings = json.loads((ROOT / 'private/device-provisioning.local.json').read_text(encoding='utf-8'))
        topic = f"btclone/{settings['prefix']}/{settings['masterId']}/raw"
        print('Checking exact configured raw topic; no payload published')
        for role in ('master', 'slave'):
            for client_id in (settings[role]['clientId'], settings[role]['username']):
                c = Client(settings['broker'], 8883, client_id, settings[role], ROOT / 'private/emqxsl-ca.crt')
                result = c.subscribe(topic) if not c.code.is_failure else [c.code]
                print(role, 'client_id=' + client_id, 'SUBACK=' + str(result[0]), flush=True)
                c.close()
    elif args.auth_only:
        settings = json.loads((ROOT / 'private/device-provisioning.local.json').read_text(encoding='utf-8'))
        for role in ('master', 'slave'):
            c = Client(settings['broker'], 8883, role + '-auth-' + secrets.token_hex(3), settings[role], ROOT / 'private/emqxsl-ca.crt')
            print(role, str(c.code), flush=True)
            c.close()
    else:
        run(args.cloud, args.port, args.native, args.negative)
