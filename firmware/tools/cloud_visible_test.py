"""Leave a clearly marked retained status message for the user's console.
Does not publish a raw BLE snapshot or any private device address/sample.
"""
import json
from pathlib import Path
import time
from mqtt_loopback import Client

root = Path(__file__).resolve().parents[1]
private = root / 'private'
d = json.loads((private / 'device-provisioning.local.json').read_text(encoding='utf-8'))
topic = f"btclone/{d['prefix']}/{d['masterId']}/status"
message = {
    'v': 1, 'device_id': d['masterId'], 'test': True,
    'kind': 'software_connectivity_test',
    'message': 'MQTT 云端连通性测试；非真实蓝牙采集，不用于蓝牙重播',
    'tested_at': int(time.time()), 'qos': 1,
    'hardware_tested': False,
    'result': '主端发布、双从端接收、保留消息、重连、权限检查已通过软件测试'
}
publisher = Client(d['broker'], 8883, 'console-status-test', d['master'], private / 'emqxsl-ca.crt')
subscriber = None
try:
    if publisher.code.is_failure:
        raise RuntimeError('主端认证未通过')
    publisher.publish(topic, json.dumps(message, ensure_ascii=False))
    subscriber = Client(d['broker'], 8883, 'console-status-check', d['slave'], private / 'emqxsl-ca.crt')
    if subscriber.code.is_failure or subscriber.subscribe(topic)[0].is_failure:
        raise RuntimeError('从端状态订阅未通过')
    received = subscriber.message()
    assert received.retain and json.loads(received.payload) == message
    report = {'topic': topic, 'retained': True, 'received_after_late_subscribe': True, 'message': message}
    (root.parent / '.dev-evidence/cloud-visible-status.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps(report, ensure_ascii=False, indent=2))
finally:
    publisher.close()
    if subscriber:
        subscriber.close()
