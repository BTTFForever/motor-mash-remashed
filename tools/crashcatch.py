"""Minimal Win32 debugger for MotorMsh.exe (32-bit) run from 64-bit Python.

Launches the game under DEBUG_ONLY_THIS_PROCESS, logs DLL loads, and on an access
violation inside the exe image dumps registers and the game's diagnostic globals.
Usage: python -I crashcatch.py <path-to-MotorMsh.exe> <log-file>
"""
import ctypes as C, ctypes.wintypes as W, os, struct, sys, time

k32 = C.WinDLL("kernel32", use_last_error=True)
DEBUG_ONLY_THIS_PROCESS = 0x2
EXCEPTION_DEBUG_EVENT, CREATE_PROCESS_DEBUG_EVENT, EXIT_PROCESS_DEBUG_EVENT, LOAD_DLL_DEBUG_EVENT = 1, 3, 5, 6
DBG_CONTINUE, DBG_EXCEPTION_NOT_HANDLED = 0x00010002, 0x80010001

class STARTUPINFO(C.Structure):
    _fields_ = [("cb", W.DWORD), ("r", W.LPWSTR), ("d", W.LPWSTR), ("t", W.LPWSTR)] + \
               [(n, W.DWORD) for n in "x y xs ys xc yc fill flags".split()] + \
               [("show", W.WORD), ("cbr2", W.WORD), ("r2", C.c_void_p), ("i", W.HANDLE), ("o", W.HANDLE), ("e", W.HANDLE)]
class PROCINFO(C.Structure):
    _fields_ = [("hProcess", W.HANDLE), ("hThread", W.HANDLE), ("pid", W.DWORD), ("tid", W.DWORD)]
class DEBUG_EVENT(C.Structure):
    _fields_ = [("code", W.DWORD), ("pid", W.DWORD), ("tid", W.DWORD), ("pad", W.DWORD), ("u", C.c_byte * 160)]

class WOW64_CONTEXT(C.Structure):
    _fields_ = [("ContextFlags", W.DWORD)] + [(f"dr{i}", W.DWORD) for i in range(6)] + \
               [("fs", C.c_byte * 112)] + [(n, W.DWORD) for n in
               "SegGs SegFs SegEs SegDs Edi Esi Ebx Edx Ecx Eax Ebp Eip SegCs EFlags Esp SegSs".split()] + \
               [("ext", C.c_byte * 512)]

def rd(h, addr, n):
    buf = C.create_string_buffer(n); got = C.c_size_t()
    ok = k32.ReadProcessMemory(h, C.c_void_p(addr), buf, n, C.byref(got))
    return buf.raw[:got.value] if ok else None

def u32(h, a):
    b = rd(h, a, 4); return struct.unpack("<I", b)[0] if b and len(b) == 4 else None

def path_from_handle(hfile):
    buf = C.create_unicode_buffer(1024)
    n = k32.GetFinalPathNameByHandleW(W.HANDLE(hfile), buf, 1024, 0)
    return buf.value if n else "?"

WATCH = {  # game globals worth dumping (from REA decomp)
    "last_hresult 0x62fa40": 0x62FA40,
    "frames_created 0x487ff8": 0x487FF8,
    "meshes_loaded 0x487ffc": 0x487FFC,
    "IDirect3DRM2 0x54af00": 0x54AF00,
    "scene_frame 0x54af08": 0x54AF08,
    "camera_frame 0x54af0c": 0x54AF0C,
    "overlay_surf 0x504fa0": 0x504FA0,
    "backbuffer 0x4891e4": 0x4891E4,
    "track_index 0x634c44": 0x634C44,
}

def main(exe, log):
    out = open(log, "w", buffering=1)
    def p(*a): print(*a); print(*a, file=out)
    si = STARTUPINFO(); si.cb = C.sizeof(si); pi = PROCINFO()
    if not k32.CreateProcessW(exe, None, None, None, False, DEBUG_ONLY_THIS_PROCESS, None,
                              os.path.dirname(exe), C.byref(si), C.byref(pi)):
        sys.exit(f"CreateProcess failed {C.get_last_error()}")
    p(f"started pid {pi.pid}")
    hproc = pi.hProcess; threads = {}
    modules = {0x400000: "MotorMsh.exe"}
    def where(a):
        bases = [b for b in modules if b <= a]
        if not bases: return f"{a:#x}"
        b = max(bases); return f"{modules[b]}+{a - b:#x}"
    ev = DEBUG_EVENT()
    while k32.WaitForDebugEvent(C.byref(ev), 0xFFFFFFFF):
        status = DBG_CONTINUE
        raw = bytes(ev.u)
        if ev.code == CREATE_PROCESS_DEBUG_EVENT:
            threads[ev.tid] = struct.unpack_from("<Q", raw, 16)[0]
        elif ev.code == 2:  # CREATE_THREAD
            threads[ev.tid] = struct.unpack_from("<Q", raw, 0)[0]
        elif ev.code == LOAD_DLL_DEBUG_EVENT:
            hfile, base = struct.unpack_from("<QQ", raw, 0)
            name = path_from_handle(hfile)
            modules[base] = name.rsplit("\\", 1)[-1]
            if any(s in name.lower() for s in ("ddraw", "d3d", "dsound", "dinput", "dplay", "winmm", "mmcda")):
                p(f"DLL {base:#010x} {name}")
            if hfile: k32.CloseHandle(W.HANDLE(hfile))
        elif ev.code == EXCEPTION_DEBUG_EVENT:
            code, flags = struct.unpack_from("<II", raw, 0)
            addr = struct.unpack_from("<Q", raw, 16)[0]
            nparams = struct.unpack_from("<I", raw, 24)[0]
            info = struct.unpack_from("<QQ", raw, 32) if nparams >= 2 else (0, 0)
            first = struct.unpack_from("<I", raw, 152)[0]
            if code in (0x80000003, 0x4000001F):  # initial breakpoints (native + WOW64)
                pass
            else:
                status = DBG_EXCEPTION_NOT_HANDLED
                if code != 0xC0000005 and (not first or code >= 0xC0000000) and code not in (0xE06D7363,):
                    p(f"\n=== EXCEPTION {code:#010x} at {where(addr)} first_chance={first}")
                if code == 0xC0000005:
                    p(f"\n=== ACCESS VIOLATION at {where(addr)} ({'read' if info[0]==0 else 'write'} of {info[1]:#x}) first_chance={first}")
                    ctx = WOW64_CONTEXT(); ctx.ContextFlags = 0x10007
                    ht = k32.OpenThread(0x1FFFFF, False, ev.tid)
                    if k32.Wow64GetThreadContext(W.HANDLE(ht), C.byref(ctx)):
                        p(" ".join(f"{r}={getattr(ctx, r):08x}" for r in "Eax Ebx Ecx Edx Esi Edi Ebp Esp Eip".split()))
                        stack = rd(hproc, ctx.Esp, 64 * 4) or b""
                        rets = [v for v in struct.unpack(f"<{len(stack)//4}I", stack) if 0x401000 <= v < 0x43D000]
                        p("stack game code ptrs:", " ".join(f"{v:#x}" for v in rets[:24]))
                        p("eip in", where(ctx.Eip))
                        raw_stack = rd(hproc, ctx.Esp - 0x100, 0x180) or b""
                        for k in range(0, len(raw_stack), 32):
                            chunk = raw_stack[k:k + 32]
                            p(f"  {ctx.Esp - 0x100 + k:08x}: {chunk.hex(' ')}  "
                              + "".join(chr(c) if 32 <= c < 127 else '.' for c in chunk))
                    k32.CloseHandle(W.HANDLE(ht))
                    for name, a in WATCH.items():
                        v = u32(hproc, a); p(f"  {name:28s} = {v:#010x}" if v is not None else f"  {name}: unreadable")
                    grid = rd(hproc, 0x488898, 64 * 4)
                    if grid:
                        vals = struct.unpack("<64I", grid)
                        p("  frame grid 0x488898 nonzero:", sum(1 for v in vals if v), "/ 64")
                    if not first:
                        p("second chance -> process will terminate")
        elif ev.code == EXIT_PROCESS_DEBUG_EVENT:
            p(f"process exited code {struct.unpack_from('<I', raw, 0)[0]:#x}")
            k32.ContinueDebugEvent(ev.pid, ev.tid, DBG_CONTINUE)
            break
        k32.ContinueDebugEvent(ev.pid, ev.tid, status)

if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
