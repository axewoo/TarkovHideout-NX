#!/usr/bin/env python3
"""Copies the published data branch files into OUT_DIR for bundling in romfs.

Usage: fetch_bundle.py OUT_DIR
"""
import json
import os
import sys
import urllib.request

BASE = "https://raw.githubusercontent.com/axewoo/TarkovHideout-NX/data/"


def get(name, out):
    req = urllib.request.Request(BASE + name, headers={"User-Agent": "TarkovHideout-NX-build"})
    with urllib.request.urlopen(req, timeout=60) as r:
        body = r.read()
    path = os.path.join(out, name)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(body)
    return body


def main():
    out = sys.argv[1]
    images = set()
    for mode in ("pve", "pvp"):
        get(f"hideout-{mode}.json", out)
        maps = json.loads(get(f"maps-{mode}.json", out))
        images.update(m["img"] for m in maps["maps"])
    get("icons.png", out)
    for img in sorted(images):
        get(img, out)
    print(f"bundled hideout data, icons and {len(images)} map images")


if __name__ == "__main__":
    main()
