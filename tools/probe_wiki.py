#!/usr/bin/env python3
"""Prints how wiki quest pages embed images (CI debugging aid)."""
import json, re, urllib.parse, urllib.request

UA = "TarkovHideout-NX data probe"
API = "https://escapefromtarkov.fandom.com/api.php"


def get(url, accept=None):
    h = {"User-Agent": UA}
    if accept:
        h["Accept"] = accept
    with urllib.request.urlopen(urllib.request.Request(url, headers=h), timeout=30) as r:
        return r.status, dict(r.headers), r.read()


for page in ("Debut", "Delivery from the Past", "Gunsmith - Part 1", "Shortage"):
    q = urllib.parse.urlencode({"action": "parse", "page": page, "prop": "text|images", "format": "json", "redirects": 1})
    st, hd, body = get(f"{API}?{q}")
    d = json.loads(body)["parse"]
    html = d["text"]["*"]
    print(f"=== {page}: {len(html)} chars html, images {d.get('images')[:20]}")
    for m in re.finditer(r"<(h2|h3|figure|img|div class=\"thumb|div class=\"wikia-gallery|aside|table)[^>]*>", html):
        print("   ", m.group(0)[:300])
    i = html.find("<figure")
    print("FIGURE SAMPLE:", html[i:i + 1500] if i >= 0 else None)
    i = html.find("gallery")
    print("GALLERY SAMPLE:", html[max(0, i - 200):i + 1500] if i >= 0 else None)
    img = re.search(r'(?:data-src|src)="(https://static\.wikia\.nocookie\.net/[^"]+)"', html)
    if img:
        url = img.group(1)
        for acc in (None, "*/*", "image/jpeg,image/png"):
            st, hd, b = get(url, acc)
            print("IMG", acc, st, hd.get("Content-Type"), len(b), url)
