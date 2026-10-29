#!/usr/bin/env python3
"""Prints the shape of upstream payloads the data build depends on (CI debugging aid)."""
import json, sys, urllib.request
sys.path.insert(0, __file__.rsplit('/', 1)[0])
import build_data as b

def show(label, obj, n=1500):
    print(f"--- {label}: {json.dumps(obj)[:n]}")

maps = b.fetch_translated("pve", "maps")
show("maps keys", list(maps.keys()) if isinstance(maps, dict) else type(maps).__name__, 300)
recs = b.records(maps.get("maps") if isinstance(maps, dict) else maps)
print("map count", len(recs))
for m in recs[:40]:
    print("map", m.get("id"), m.get("normalizedName"), m.get("name"), sorted(m.keys()))
cust = next((m for m in recs if m.get("normalizedName") == "customs"), recs[0] if recs else {})
for k in ("extracts", "transits", "bosses", "spawns"):
    v = cust.get(k)
    show(f"customs.{k} ({len(v) if isinstance(v, list) else type(v).__name__})", v[:3] if isinstance(v, list) else v, 1200)
items = b.fetch_translated("pve", "items")
show("items keys", list(items.keys()), 400)
first = next(iter(b.records(items.get("items"))), {})
show("item sample", {k: first.get(k) for k in ("id", "name", "shortName", "iconLink", "gridImageLink", "baseImageLink", "width", "height", "backgroundColor")})
show("playerLevels", (items.get("playerLevels") or [])[:3])
show("skills", (items.get("skills") or [])[:3] if isinstance(items.get("skills"), list) else items.get("skills"), 600)
for url in ("https://players.tarkov.dev/profile/8560316.json", "https://players.tarkov.dev/pve/8560316.json"):
    try:
        req = urllib.request.Request(url, headers={"User-Agent": b.USER_AGENT})
        with urllib.request.urlopen(req, timeout=30) as r:
            p = json.loads(r.read())
        show(f"{url} top keys", sorted(p.keys()), 600)
        show("info", p.get("info"), 800)
        show("skills.Common[:3]", (p.get("skills") or {}).get("Common", [])[:3], 500)
        show("pmcStats", p.get("pmcStats"), 1200)
        show("updated", p.get("updated"), 100)
        h = p.get("hideout") or {}
        show("hideout keys", sorted(h.keys()) if isinstance(h, dict) else type(h).__name__, 400)
        if isinstance(h, dict):
            for k, v in h.items():
                show(f"hideout.{k}", v[:3] if isinstance(v, list) else v, 700)
        show("achievements", dict(list((p.get("achievements") or {}).items())[:3]) if isinstance(p.get("achievements"), dict) else p.get("achievements"), 300)
        show("scavStats", p.get("scavStats"), 400)
    except Exception as e:
        print("profile fetch failed", url, e)
