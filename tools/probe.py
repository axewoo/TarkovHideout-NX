#!/usr/bin/env python3
"""Prints the shape of upstream payloads the data build depends on (CI debugging aid)."""
import json, sys, urllib.request
from collections import Counter
sys.path.insert(0, __file__.rsplit('/', 1)[0])
import build_data as b

def show(label, obj, n=1500):
    print(f"--- {label}: {json.dumps(obj, ensure_ascii=False)[:n]}")

def get(url):
    req = urllib.request.Request(url, headers={"User-Agent": b.USER_AGENT})
    with urllib.request.urlopen(req, timeout=30) as r:
        return r.status, r.headers.get("Content-Type"), r.read()

items = b.fetch_translated("pve", "items")
its = b.records(items.get("items"))
print("item types", Counter(t for i in its for t in (i.get("types") or [])).most_common(40))
ammo = next((i for i in its if "ammo" in (i.get("types") or [])), None)
show("ammo item", ammo, 2500)
key = next((i for i in its if "keys" in (i.get("types") or [])), None)
show("key item", key, 2000)
show("item keys", sorted(its[0].keys()) if its else None, 1000)
show("first item full", its[0], 3000)
show("items top-level other", {k: str(v)[:400] for k, v in items.items() if k != "items"}, 3000)
tr = b.fetch_json("pve/traders")
show("trader sample", b.records(tr.get("data"))[:1], 1500)
for ep in ("pve/item_prices", "pve/prices", "pve/barters"):
    try:
        d = b.fetch_json(ep)
        show(ep, {k: (v if not isinstance(v, (list, dict)) else str(v)[:600]) for k, v in d.items()}, 1200)
    except Exception as e:
        print("endpoint failed", ep, e)

for url in ("https://escapefromtarkov.fandom.com/api.php?action=parse&page=Gunsmith_-_Part_1&prop=wikitext&format=json",
            "https://escapefromtarkov.fandom.com/api.php?action=query&prop=extracts&explaintext=1&titles=Debut&format=json",
            "https://escapefromtarkov.fandom.com/wiki/Debut?action=raw",
            "https://escapefromtarkov.wiki.gg/api.php?action=parse&page=Debut&prop=wikitext&format=json",
            "https://wiki.escapefromtarkov.com/api.php?action=parse&page=Debut&prop=wikitext&format=json"):
    try:
        st, ct, body = get(url)
        print("wiki", st, ct, len(body), url)
        print("   ", body[:1500].decode("utf-8", "replace").replace("\n", "\\n"))
    except Exception as e:
        print("wiki failed", url, e)
