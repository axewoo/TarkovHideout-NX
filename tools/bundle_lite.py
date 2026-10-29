#!/usr/bin/env python3
"""Copies the small part of the data branch that a Lite build bundles.

Lite builds (the Lite NRO and the Android app) ship the hideout, quest, ammo and
item data, the icon atlases and the map picker thumbnails. Map images, sharp map
pieces and wiki pictures download the first time they are viewed.

usage: bundle_lite.py <data branch checkout> <output folder>
Also writes files.txt, the list of bundled files, which Android uses to unpack them.
"""
import pathlib
import shutil
import sys


def main():
    src, out = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
    files = [p for p in src.iterdir() if p.is_file() and p.suffix in (".json", ".jpg")]
    maps = src / "maps"
    if maps.is_dir():
        for d in sorted(maps.iterdir()):
            for name in ("thumb.jpg", "meta.json"):
                if (d / name).is_file():
                    files.append(d / name)
    listed = []
    for f in sorted(files):
        rel = f.relative_to(src).as_posix()
        dst = out / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(f, dst)
        listed.append(rel)
    (out / "files.txt").write_text("\n".join(listed) + "\n")
    size = sum((out / r).stat().st_size for r in listed)
    print(f"bundled {len(listed)} files, {size / 1e6:.1f} MB")


if __name__ == "__main__":
    main()
