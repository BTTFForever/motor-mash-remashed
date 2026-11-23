"""Read Motor Mash display-mode state from the running game (read-only)."""
import ctypes as C, struct, subprocess, sys
k = C.WinDLL("kernel32")
pid = int(subprocess.check_output('powershell -NoProfile -Command "(Get-Process MotorMsh_dbg).Id"').strip())
h = k.OpenProcess(0x10 | 0x400, False, pid)
def rd(a, n):
    b = C.create_string_buffer(n); g = C.c_size_t()
    k.ReadProcessMemory(h, C.c_void_p(a), b, n, C.byref(g)); return b.raw[:g.value]
u32 = lambda a: struct.unpack("<I", rd(a, 4))[0]
i16 = lambda a: struct.unpack("<h", rd(a, 2))[0]
n = u32(0x617620)
print("enumerated modes:", n, " current index:", u32(0x6172ec))
for i in range(n):
    w, hh, bpp, _ = struct.unpack("<4I", rd(0x6172F0 + i * 16, 16))
    print(f"  [{i:2}] {w}x{hh}x{bpp}")
print("cur width/height 0x62fa48/0x62fb08:", i16(0x62FA48), i16(0x62FB08), " bpp 0x61ba34:", i16(0x61BA34))
print("windowed flag 0x62fa5c:", u32(0x62FA5C), " mode 0x617650/54:", u32(0x617650), u32(0x617654))
