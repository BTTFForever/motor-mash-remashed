"""Drive Motor Mash for automated testing: focus the window, press keys, take screenshots.

Keys are sent with SendInput using hardware scan codes, because the game reads the
keyboard through DirectInput (which ignores virtual-key-only input). The mmcda.dll debug
menu polls GetAsyncKeyState, which sees the same events.

Usage (one or more steps, run in order):
  python -I gamectl.py focus
  python -I gamectl.py key F10 DOWN DOWN RIGHT          # tap keys (0.15 s apart)
  python -I gamectl.py hold UP 2.5                      # hold a key for N seconds
  python -I gamectl.py wait 1.5
  python -I gamectl.py shot C:\\path\\out.png           # full primary screen, PNG
Steps can be chained:  focus key F10 wait 0.5 shot a.png
"""
import ctypes, ctypes.wintypes as W, struct, sys, time, zlib

u32 = ctypes.WinDLL("user32", use_last_error=True)
g32 = ctypes.WinDLL("gdi32")

# scan codes (set 1); E0-prefixed keys flagged
SC = {
    "ESC": 0x01, "ESCAPE": 0x01, "1": 0x02, "2": 0x03, "3": 0x04, "4": 0x05, "5": 0x06, "6": 0x07,
    "7": 0x08, "8": 0x09, "9": 0x0A, "0": 0x0B, "TAB": 0x0F, "Q": 0x10, "W": 0x11, "E": 0x12,
    "R": 0x13, "T": 0x14, "Y": 0x15, "U": 0x16, "I": 0x17, "O": 0x18, "P": 0x19, "ENTER": 0x1C,
    "RETURN": 0x1C, "CTRL": 0x1D, "A": 0x1E, "S": 0x1F, "D": 0x20, "F": 0x21, "G": 0x22, "H": 0x23,
    "J": 0x24, "K": 0x25, "L": 0x26, "SHIFT": 0x2A, "Z": 0x2C, "X": 0x2D, "C": 0x2E, "V": 0x2F,
    "B": 0x30, "N": 0x31, "M": 0x32, "SPACE": 0x39, "F1": 0x3B, "F2": 0x3C, "F3": 0x3D, "F4": 0x3E,
    "F5": 0x3F, "F6": 0x40, "F7": 0x41, "F8": 0x42, "F9": 0x43, "F10": 0x44, "F11": 0x57, "F12": 0x58,
}
EXT = {"UP": 0x48, "DOWN": 0x50, "LEFT": 0x4B, "RIGHT": 0x4D, "HOME": 0x47, "END": 0x4F,
       "PGUP": 0x49, "PGDN": 0x51, "INSERT": 0x52, "DELETE": 0x53}

class KI(ctypes.Structure):
    _fields_ = [("wVk", W.WORD), ("wScan", W.WORD), ("dwFlags", W.DWORD), ("time", W.DWORD), ("extra", ctypes.c_void_p)]
class INPUT(ctypes.Structure):
    _fields_ = [("type", W.DWORD), ("ki", KI), ("pad", ctypes.c_byte * 8)]

KEYEVENTF_EXTENDEDKEY, KEYEVENTF_KEYUP, KEYEVENTF_SCANCODE = 1, 2, 8

def _send(name, up):
    name = name.upper()
    if name in EXT: sc, fl = EXT[name], KEYEVENTF_SCANCODE | KEYEVENTF_EXTENDEDKEY
    elif name in SC: sc, fl = SC[name], KEYEVENTF_SCANCODE
    else: raise SystemExit(f"unknown key {name}")
    i = INPUT(type=1, ki=KI(0, sc, fl | (KEYEVENTF_KEYUP if up else 0), 0, None))
    u32.SendInput(1, ctypes.byref(i), ctypes.sizeof(INPUT))

def tap(name, hold=0.08):
    _send(name, False); time.sleep(hold); _send(name, True)

def find_game():
    found = []
    @ctypes.WINFUNCTYPE(W.BOOL, W.HWND, W.LPARAM)
    def cb(h, _):
        buf = ctypes.create_unicode_buffer(256)
        u32.GetWindowTextW(h, buf, 256)
        if buf.value == "Motor Mash" and u32.IsWindowVisible(h): found.append(h)
        return True
    u32.EnumWindows(cb, 0)
    return found[0] if found else None

def focus():
    h = find_game()
    if not h: raise SystemExit("game window not found")
    # tap ALT so Windows allows the foreground change from a background process
    u32.keybd_event(0x12, 0, 0, 0); u32.keybd_event(0x12, 0, 2, 0)
    u32.ShowWindow(h, 9)
    u32.SetForegroundWindow(h)
    time.sleep(0.3)
    print("focused" if u32.GetForegroundWindow() == h else "focus FAILED")

def shot(path, virtual=False):
    x0, y0 = (u32.GetSystemMetrics(76), u32.GetSystemMetrics(77)) if virtual else (0, 0)
    w, h = (u32.GetSystemMetrics(78), u32.GetSystemMetrics(79)) if virtual else (u32.GetSystemMetrics(0), u32.GetSystemMetrics(1))
    sdc = u32.GetDC(0); mdc = g32.CreateCompatibleDC(sdc)
    bmp = g32.CreateCompatibleBitmap(sdc, w, h); g32.SelectObject(mdc, bmp)
    g32.BitBlt(mdc, 0, 0, w, h, sdc, x0, y0, 0x00CC0020)
    class BIH(ctypes.Structure):
        _fields_ = [("size", W.DWORD), ("w", W.LONG), ("h", W.LONG), ("planes", W.WORD), ("bpp", W.WORD),
                    ("comp", W.DWORD), ("isize", W.DWORD), ("xp", W.LONG), ("yp", W.LONG), ("cu", W.DWORD), ("ci", W.DWORD)]
    bih = BIH(ctypes.sizeof(BIH), w, -h, 1, 24, 0, 0, 0, 0, 0, 0)
    stride = (w * 3 + 3) & ~3
    buf = ctypes.create_string_buffer(stride * h)
    g32.GetDIBits(mdc, bmp, 0, h, buf, ctypes.byref(bih), 0)
    g32.DeleteObject(bmp); g32.DeleteDC(mdc); u32.ReleaseDC(0, sdc)
    raw = bytearray()
    for y in range(h):
        row = buf.raw[y * stride: y * stride + w * 3]
        raw.append(0)
        raw += bytes(b for px in range(w) for b in (row[px * 3 + 2], row[px * 3 + 1], row[px * 3]))
    def chunk(t, d): return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + \
          chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + chunk(b"IEND", b"")
    open(path, "wb").write(png)
    print("shot", path)

def main(args):
    i = 0
    while i < len(args):
        a = args[i].lower(); i += 1
        if a == "focus": focus()
        elif a == "wait": time.sleep(float(args[i])); i += 1
        elif a == "shot": shot(args[i]); i += 1
        elif a == "vshot": shot(args[i], True); i += 1          # all monitors
        elif a == "hold":
            _send(args[i], False); time.sleep(float(args[i + 1])); _send(args[i], True); i += 2
        elif a == "key":
            while i < len(args) and args[i].lower() not in ("focus", "wait", "shot", "vshot", "hold", "key"):
                tap(args[i]); time.sleep(0.15); i += 1
        else: raise SystemExit(f"unknown step {a}")

if __name__ == "__main__":
    main(sys.argv[1:])
