from pathlib import Path
import gzip

root = Path(__file__).resolve().parents[1]
page = (root / 'web/index.html').read_text(encoding='utf-8')
assert ')BTCWEB"' not in page
(root / 'include/web_page.h').write_text(
    '#pragma once\n#include <pgmspace.h>\nstatic const char WEB_PAGE[] PROGMEM = R"BTCWEB('
    + page + ')BTCWEB";\n'
    + 'static const unsigned char WEB_PAGE_GZIP[] PROGMEM = {'
    + ','.join(str(x) for x in gzip.compress(page.encode('utf-8'),compresslevel=9,mtime=0)) + '};\n', encoding='utf-8')
