"""recover_entries.py <MotorMsh.exe> <entries.txt>

Writes the function entries the first Ghidra analysis missed, for RecoverMissed.java:
the front-end page table (33 page handlers at 0x450ec0) and the per-track trap routine
tables (setup 0x454d98, update 0x454f38; 6 worlds x 16 slots). Reads only addresses.

Then (Ghidra 12.2 needs JDK 25 in JAVA_HOME):
  analyzeHeadless <projdir> mm -import MotorMsh.exe -overwrite -scriptPath tools/ghidra
      -postScript RecoverMissed.java entries.txt <out folder> -deleteProject
"""
import struct, sys

d = open(sys.argv[1], 'rb').read()
pe = struct.unpack_from('<I', d, 0x3c)[0]
n = struct.unpack_from('<H', d, pe + 6)[0]
opt = pe + 24
secs = [struct.unpack_from('<8sIIII', d, opt + 224 + 40 * i) for i in range(n)]


def off(va):
    r = va - 0x400000
    for _, vs, va0, rs, ro in secs:
        if va0 <= r < va0 + rs:
            return r - va0 + ro


def rd(va):
    return struct.unpack_from('<I', d, off(va))[0]


ents = {rd(a) for a in range(0x450EC0, 0x450F44, 4)}           # front-end pages
ents |= {rd(a) for a in range(0x454D98, 0x4550DC, 4)}          # per-track trap setup/update
open(sys.argv[2], 'w').write('\n'.join('%x' % e for e in sorted(ents) if 0x401000 <= e < 0x43D000))
print(len(ents), 'entries')
