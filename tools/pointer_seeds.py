"""Missed function entries found from pointers to them.

An entry qualifies when a real function pointer (a word in .rdata/.data, or a push/mov immediate)
refers to it, it is a clean instruction start on a 16-byte boundary, and the instruction before it
ends control flow (ret/jmp) followed by padding. Code bytes are not used as pointers: jump tables
and hand-aligned CRT case labels look like function starts.

Usage: pointer_seeds.py <pipeline dir> <xbe> <out.json>
"""
import json
from pathlib import Path
import re
import struct
import sys

pipe, xbe, out = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
starts = {int(f['start'], 16) for f in json.loads((pipe/'disasm/functions.json').read_text())}
line = re.compile(r'^  0x([0-9A-F]{8})  ([0-9a-f]+)\s+(\S+)\s*(.*?)\s*$')
imm = re.compile(r'(?:^|, )0x([0-9a-f]+)$')
insns, code_imms, end = [], set(), 0
for asm in sorted((pipe/'disasm/asm').glob('*.asm')):
    for text in asm.read_text(errors='replace').splitlines():
        if text.startswith('; Function:'):
            end = 0
        m = line.match(text)
        if m and int(m[1], 16) >= end:          # skip overlapping decodes
            a = int(m[1], 16); insns.append((a, m[3])); end = a + len(m[2]) // 2
            if m[3] in ('push', 'mov') and imm.search(m[4]):
                code_imms.add(int(imm.search(m[4])[1], 16))
insns.sort()
prev_end = {}
last, padded = None, False
for a, mn in insns:
    prev_end[a] = (last, padded)
    if mn in ('nop', 'int3'):
        padded = True
    else:
        last, padded = mn, False
d = xbe.read_bytes()
base = struct.unpack_from('<I', d, 0x104)[0]
sh = struct.unpack_from('<I', d, 0x120)[0] - base
values, vtable_slots = set(code_imms), set()
text_end = 0x11000 + 0x2061F0
for k in range(struct.unpack_from('<I', d, 0x11C)[0]):
    _, va, _, raw, size, name = struct.unpack_from('<6I', d, sh + k * 56)
    if d[name - base:d.index(b'\x00', name - base)] in (b'.rdata', b'.data'):
        words = [struct.unpack_from('<I', d, raw + o)[0] for o in range(0, size - 3, 4)]
        values |= set(words)
        # A vtable slot beside a known entry, pointing into bytes decoded as one instruction: the
        # decode is wrong there, since real code has no entry inside an instruction.
        for i, v in enumerate(words):
            if 0x11000 <= v < text_end and v % 16 == 0 and v not in prev_end and v not in starts and (
                    (i and words[i - 1] in starts) or (i + 1 < len(words) and words[i + 1] in starts)):
                vtable_slots.add(v)
seeds = sorted({a for a in values if a in prev_end and a not in starts and a % 16 == 0
                and prev_end[a][1] and prev_end[a][0] in ('ret', 'retn', 'jmp')} | vtable_slots)
json.dump([{'start': f'0x{s:08x}'} for s in seeds], out.open('x'), indent=1)
print(len(seeds), 'pointer-referenced entries:', len(vtable_slots), 'vtable slots into undecoded/junk bytes,',
      len(seeds) - len(vtable_slots), 'after ret/jmp padding')
assert not any(0x1DD600 <= s < 0x1DD93D for s in seeds), 'memcpy case labels must not qualify'
