"""Check actual compiled RTC placement; never opens serial ports."""
import argparse
import json
import os
import re
import shutil
import subprocess
from pathlib import Path

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build-dir', type=Path)
args = parser.parse_args()
base = args.build_dir or Path(os.environ.get('PLATFORMIO_BUILD_DIR', str(Path.home()/'.platformio/btclone-v030-build') if os.name == 'nt' else str(root/'.pio/build')))
source = (root/'src/main.cpp').read_text(encoding='utf-8')
assert re.search(r'RTC_NOINIT_ATTR\s+uint32_t\s+nextBootMaintenance\s*;', source)
assert 'resetReason==ESP_RST_SW && requestedMaintenance' in source
assert 'nextBootMaintenance=0;' in source
tool = shutil.which('riscv32-esp-elf-objdump')
if not tool:
    candidates = list((Path.home()/'.platformio/packages').glob('toolchain-riscv32-esp/bin/riscv32-esp-elf-objdump*'))
    tool = str(next(p for p in candidates if p.suffix != '.py'))
roles = []
for role in ['master_ml307', 'slave_c3']:
    elf = base/role/'firmware.elf'
    result = subprocess.check_output([tool, '-t', str(elf)], text=True)
    symbols = [line for line in result.splitlines() if 'nextBootMaintenance' in line]
    assert len(symbols) == 1 and '.rtc_noinit' in symbols[0], role + ': restart intent is initialized by bootloader'
    lines = subprocess.check_output([tool, '-h', str(elf)], text=True).splitlines()
    index = next(i for i, line in enumerate(lines) if re.search(r'\s\.rtc_noinit\s', line))
    assert 'LOAD' not in lines[index+1], role + ': noinit section is loadable'
    roles.append({'role': role, 'symbolSection': '.rtc_noinit', 'loadedFromImage': False})
report = {'kind': 'compiled_elf_storage_check', 'softwareRestartIntentRetained': True,
          'roles': roles, 'hardwareAccepted': False,
          'scope': 'ELF placement and source classification only; no physical reset tested'}
out = root/'dist/v0.3.7-dev/evidence/boot-intent-r1.json'
out.parent.mkdir(parents=True, exist_ok=True)
out.write_text(json.dumps(report, indent=2), encoding='utf-8')
print(json.dumps(report))
