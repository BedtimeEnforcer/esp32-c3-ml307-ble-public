"""Compile the actual firmware owner with fake hardware; never opens a port."""
from pathlib import Path
import subprocess,sys,shutil,os
root=Path(__file__).resolve().parents[1]
out=root.parent/'.dev-evidence';out.mkdir(exist_ok=True)
cxx=os.environ.get('CXX') or shutil.which('g++') or 'D:/WorkBuddyEnv/tools/usb-native/w64devkit/bin/g++.exe'
log=''
for role,name in ((1,'runtime_test'),(0,'slave_runtime_test')):
    cmd=[cxx,'-std=c++17','-O1','-Wall','-Wextra','-Wno-unused-function','-Wno-unused-variable','-static-libgcc','-static-libstdc++',f'-DROLE_MASTER={role}']
    cmd += ['-I'+str(root/p) for p in ('tests/fakes','tests','include','.pio/libdeps/master_ml307/ArduinoJson/src')]
    cmd += [str(root/'src'/p) for p in ('core.cpp','protocol.cpp','remote.cpp','modem.cpp','persistence.cpp','subscriber.cpp')]
    exe=out/(name+'.exe');cmd += [str(root/'tests'/(name+'.cpp')),'-o',str(exe)]
    subprocess.run(cmd,check=True)
    r=subprocess.run([str(exe)],capture_output=True,text=True,encoding='utf-8')
    print(r.stdout,end='');print(r.stderr,end='',file=sys.stderr);log+=r.stdout+r.stderr
    (out/'runtime-test.log').write_text(log,encoding='utf-8')
    if r.returncode:sys.exit(r.returncode)
