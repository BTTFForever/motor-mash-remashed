"""Encode game/Music/TrackNN.wav (CD rips) to Ogg Vorbis for embedding in the launcher.
Usage: python encode_music.py <wav_dir> <out_dir>

Written in small blocks: libsndfile's Vorbis encoder overflows the stack when given
a whole track in one write call."""
import os, sys
import soundfile as sf

src, out = sys.argv[1], sys.argv[2]
total = 0
for name in sorted(os.listdir(src)):
    if not name.lower().endswith(".wav"):
        continue
    dst = os.path.join(out, name[:-4] + ".ogg")
    with sf.SoundFile(os.path.join(src, name)) as inp, \
         sf.SoundFile(dst, "w", samplerate=inp.samplerate, channels=inp.channels,
                      format="OGG", subtype="VORBIS", compression_level=0.4) as outp:
        for block in inp.blocks(blocksize=4096, dtype="float32"):
            outp.write(block)
    total += os.path.getsize(dst)
    print(f"{name} -> {os.path.getsize(dst) / 1e6:.1f} MB", flush=True)
print(f"total {total / 1e6:.1f} MB")
