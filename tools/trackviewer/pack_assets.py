"""Pack MotorMashViewer's built 3D assets for the hosted track viewer.

Per world: models.json and tiles.json are copied; the ~700 texture PNGs are folded into
one tex.json of {name: data-URI}, so the page ships a handful of files instead of 4,200.

Usage: python -I pack_assets.py <MotorMashViewer/web/public/assets> <trackviewer/assets>
"""
import base64, json, os, shutil, sys

src, out = sys.argv[1], sys.argv[2]
WORLDS = ["West", "Arctic", "Natlanti", "Nightmar", "Amazon", "City"]
total = 0
for w in WORLDS:
    os.makedirs(os.path.join(out, w), exist_ok=True)
    for f in ("models.json", "tiles.json"):
        shutil.copyfile(os.path.join(src, w, f), os.path.join(out, w, f))
    tex = {}
    tdir = os.path.join(src, w, "tex")
    for fn in sorted(os.listdir(tdir)):
        if fn.lower().endswith(".png"):
            tex[fn[:-4]] = "data:image/png;base64," + base64.b64encode(open(os.path.join(tdir, fn), "rb").read()).decode()
    json.dump(tex, open(os.path.join(out, w, "tex.json"), "w"), separators=(",", ":"))
    sizes = [os.path.getsize(os.path.join(out, w, f)) for f in ("models.json", "tiles.json", "tex.json")]
    total += sum(sizes)
    print(f"{w}: {len(tex)} textures, models {sizes[0] / 1e6:.1f} MB, tex {sizes[2] / 1e6:.1f} MB")
print(f"total {total / 1e6:.1f} MB")
