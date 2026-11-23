"""Patch MotorMsh.exe (sha256 04167837...) to fix the double-Release crash on race start.

FUN_004075b0 Releases the overlay surface (global 0x504fa0) when GetSurfaceDesc or Lock
fails, but never clears the pointer; FUN_00407070 later Releases it again -> AV at 0x407080.
We NOP out the two early Releases so the surface is released exactly once by 0x407070.
Also renames the WINMM.dll import to MMCDA.dll so CD music is served by tools/mmcda.
Input must be the original disc exe; writes the patched file in place.
"""
import hashlib, sys

FILE_DELTA = 0x400C00  # VA - file offset for .text (VA 0x401000 -> raw 0x400)
RDATA_DELTA = 0x401A00  # VA - file offset for .rdata (VA 0x43d000 -> raw 0x3b600)
# (va, original bytes, replacement or None for NOPs, section delta)
PATCHES = [
    (0x407620, bytes.fromhex("8B1050FF5208"), None, FILE_DELTA),
    (0x40764B, bytes.fromhex("A1A04F5000508B08FF5108"), None, FILE_DELTA),
    # import DLL name WINMM.dll -> MMCDA.dll (CD-audio emulator, tools/mmcda)
    (0x43E5FE, b"WINMM.dll", b"MMCDA.dll", RDATA_DELTA),
    # main window style WS_POPUP|WS_VISIBLE -> WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX|WS_VISIBLE
    # (CreateWindowExA in 0x42cdf0; the game already adds +8/+46 for frame and caption)
    (0x42CEA3, bytes.fromhex("6800000090"), bytes.fromhex("680000CA10"), FILE_DELTA),
]
ORIG_SHA = "04167837e1b992d7549924725e0978c458affcc0ea330857d81ef960b9101551"

def main(path):
    data = bytearray(open(path, "rb").read())
    if hashlib.sha256(data).hexdigest() != ORIG_SHA:
        sys.exit("unexpected input hash; refusing to patch")
    for va, orig, new, delta in PATCHES:
        off = va - delta
        if data[off:off + len(orig)] != orig:
            sys.exit(f"bytes mismatch at {va:#x}: {data[off:off+len(orig)].hex()}")
        data[off:off + len(orig)] = new if new is not None else b"\x90" * len(orig)
    open(path, "wb").write(data)
    print("patched", path, hashlib.sha256(data).hexdigest())

if __name__ == "__main__":
    main(sys.argv[1])
