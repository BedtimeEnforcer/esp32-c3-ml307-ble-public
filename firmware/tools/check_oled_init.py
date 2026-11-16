"""Read-only OLED initialization regression guard. Does not compile or flash."""
from pathlib import Path
import argparse,json,re,subprocess
root=Path(__file__).resolve().parents[1]
def inspect(source):
    text=re.sub(r'/\*.*?\*/|//[^\n]*','',source,flags=re.S)
    ctor=re.findall(r'U8G2_SH1106_128X64_NONAME_F_HW_I2C\s+screen\s*\(([^;]+)\)\s*;',text)
    args=[a.strip() for a in ctor[0].split(',')] if len(ctor)==1 else []
    wire=list(re.finditer(r'\bWire\.begin\s*\(\s*SCREEN_SDA\s*,\s*SCREEN_SCL\s*\)',text))
    display=re.search(r'\bscreen\.begin\s*\(',text)
    checks={'u8g2_does_not_receive_bus_pins':args==['U8G2_R0','U8X8_PIN_NONE'],
            'one_explicit_wire_pin_initialization':len(wire)==1,
            'wire_initialized_before_display':bool(wire and display and wire[0].start()<display.start()),
            'wifi_bt_power_save_retained':not re.search(r'(?:WiFi\.setSleep\s*\(\s*false|esp_wifi_set_ps\s*\(\s*WIFI_PS_NONE)',text)}
    return checks
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--compare-tag',default='demo-v0.2.1')
args=parser.parse_args()
checks=inspect((root/'src/main.cpp').read_text(encoding='utf-8'))
baseline=subprocess.run(['git','show',f'{args.compare_tag}:firmware/src/main.cpp'],cwd=root.parent,capture_output=True)
old_rejected=None
if baseline.returncode==0:
    old_rejected=not all(inspect(baseline.stdout.decode('utf-8')).values())
    assert old_rejected,'Regression guard did not detect original double pin setup'
assert all(checks.values()),checks
report={'kind':'static_source_check','checks':checks,'old_release_rejected':old_rejected,'historical_baseline_available':baseline.returncode==0,
        'roles_covered':['master_shared_source','slave_shared_source'],
        'compiled':False,'flashed':False,'new_firmware_hardware_accepted':False}
out=root.parent/'.dev-evidence';out.mkdir(exist_ok=True)
(out/'oled-init-static-check.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
print(json.dumps(report,indent=2))
