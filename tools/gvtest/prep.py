import re, sys
src, dst, prefix = sys.argv[1], sys.argv[2], sys.argv[3]
t = open(src, 'rb').read().decode('latin-1')
out = []
for ln in t.split('\n'):
    if ln.startswith('#include'):
        continue
    if re.match(r'\s*(BYTE|WORD)\s+(GVRAM|Grp_LineBuf|Grp_LineBufSP|Grp_LineBufSP2|Pal16Adr)\[', ln):
        continue
    out.append(ln)
t = '\n'.join(out)
t = re.sub(r'\b(Grp_DrawLine\w*|GVRAM_Init|GVRAM_FastClear|GVRAM_Read|GVRAM_Write)\b', prefix + r'\1', t)
open(dst, 'w').write('#include "stub.h"\n' + t)
