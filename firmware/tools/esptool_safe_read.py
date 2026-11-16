"""Read-only esptool wrapper: small native-USB packets, one outstanding packet, MD5 verification."""
from pathlib import Path
import sys,site,struct,hashlib
tool=Path.home()/'.platformio/packages/tool-esptoolpy'
sys.path.insert(0,str(tool));site.addsitedir(str(tool/'_contrib'))
import esptool
from esptool.loader import ESPLoader
from esptool.util import FatalError
if 'read_flash' not in sys.argv[1:]:raise SystemExit('This wrapper permits read_flash only; it never writes or erases.')
original_read=ESPLoader.read_flash
def safe_read(self,offset,length,progress_fn=None):
    if not self.IS_STUB:return original_read(self,offset,length,progress_fn)
    size=128
    self._port.set_buffer_size(rx_size=262144,tx_size=65536)
    data=bytearray()
    # Bound each native-USB stream. A stalled long stream previously left an
    # unusable partial backup; every segment has its own module-side MD5.
    while len(data)<length:
        count=min(4096,length-len(data));segment=bytearray()
        self.check_command('read flash',self.ESP_READ_FLASH,struct.pack('<IIII',offset+len(data),count,size,1))
        while len(segment)<count:
            packet=self.read()
            if len(packet)!=min(size,count-len(segment)):raise FatalError('Incomplete native USB flash packet; backup rejected')
            segment.extend(packet);self.write(struct.pack('<I',len(segment)))
        digest=self.read()
        if len(digest)!=16 or hashlib.md5(segment).digest()!=digest:raise FatalError('Flash segment MD5 mismatch; backup rejected')
        data.extend(segment)
        if progress_fn:progress_fn(len(data),length)
    return bytes(data)
ESPLoader.read_flash=safe_read
esptool.main(sys.argv[1:])
