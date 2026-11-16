"""Compile and execute the same portable C++ used by both firmwares."""
from pathlib import Path
import subprocess
import sys
import shutil
import os

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT.parent / '.dev-evidence'
OUT.mkdir(exist_ok=True)
EXE = OUT / 'core_test.exe'
compiler = os.environ.get('CXX') or shutil.which('g++')
if not compiler and Path('D:/WorkBuddyEnv/tools/usb-native/w64devkit/bin/g++.exe').exists():
    compiler = 'D:/WorkBuddyEnv/tools/usb-native/w64devkit/bin/g++.exe'
prefix = [compiler] if compiler else [sys.executable, '-m', 'ziglang', 'c++']
cmd = prefix + ['-std=c++17', '-O1', '-Wall', '-Wextra', '-static-libgcc', '-static-libstdc++', '-DROLE_MASTER=1',
       '-I' + str(ROOT / 'tests/fakes'),
       '-I' + str(ROOT / 'include'), '-I' + str(ROOT / '.pio/libdeps/master_ml307/ArduinoJson/src'),
       str(ROOT / 'src/core.cpp'), str(ROOT / 'src/protocol.cpp'), str(ROOT / 'src/remote.cpp'), str(ROOT / 'src/modem.cpp'), str(ROOT / 'src/persistence.cpp'), str(ROOT / 'src/subscriber.cpp'),
       str(ROOT / 'tests/core_test.cpp'), str(ROOT / 'tests/modem_test.cpp'), str(ROOT / 'tests/subscriber_test.cpp'), str(ROOT / 'tests/remote_test.cpp'), '-o', str(EXE)]
subprocess.run(cmd, check=True)
result = subprocess.run([str(EXE)], text=True, encoding='utf-8', capture_output=True)
print(result.stdout, end='')
print(result.stderr, end='', file=sys.stderr)
(OUT / 'core-test.log').write_text(result.stdout + result.stderr, encoding='utf-8')
sys.exit(result.returncode)
