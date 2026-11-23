"""Rip the Red Book audio tracks from Motor.bin (raw 2352-byte sectors) to WAV using Motor.cue.

Usage: python -I rip_tracks.py <Motor.cue> <Motor.bin> <out_dir>
Writes TrackNN.wav (44.1 kHz, 16-bit, stereo) for every AUDIO track, from INDEX 01
up to the next track's INDEX 00 (pregap excluded), last track to end of file.
"""
import os, re, struct, sys

SECTOR = 2352

def msf(s):
    m, sec, f = (int(x) for x in s.split(":"))
    return (m * 60 + sec) * 75 + f

def main(cue, binf, out):
    tracks, cur = [], None
    for line in open(cue, encoding="latin-1"):
        if m := re.match(r"\s*TRACK\s+(\d+)\s+(\S+)", line):
            cur = {"n": int(m[1]), "type": m[2], "idx": {}}; tracks.append(cur)
        elif (m := re.match(r"\s*INDEX\s+(\d+)\s+(\d+:\d+:\d+)", line)) and cur:
            cur["idx"][int(m[1])] = msf(m[2])
    total = os.path.getsize(binf) // SECTOR
    os.makedirs(out, exist_ok=True)
    with open(binf, "rb") as f:
        for i, t in enumerate(tracks):
            if t["type"] != "AUDIO":
                continue
            start = t["idx"][1]
            end = total
            if i + 1 < len(tracks):
                nxt = tracks[i + 1]["idx"]
                end = nxt.get(0, nxt[1])
            f.seek(start * SECTOR)
            pcm = f.read((end - start) * SECTOR)
            path = os.path.join(out, f"Track{t['n']:02d}.wav")
            with open(path, "wb") as w:
                w.write(b"RIFF" + struct.pack("<I", 36 + len(pcm)) + b"WAVE")
                w.write(b"fmt " + struct.pack("<IHHIIHH", 16, 1, 2, 44100, 44100 * 4, 4, 16))
                w.write(b"data" + struct.pack("<I", len(pcm)) + pcm)
            print(f"{path}: {len(pcm) / 176400:.1f}s")

if __name__ == "__main__":
    main(*sys.argv[1:4])
