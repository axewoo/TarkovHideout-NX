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

tasks_raw = b.fetch_json("pve/tasks")
show("tasks top keys", list(tasks_raw.keys()), 300)
show("tasks translations", tasks_raw.get("translations"), 600)
tasks = b.fetch_translated("pve", "tasks")
show("tasks data type", type(tasks).__name__ + (str(list(tasks.keys())[:10]) if isinstance(tasks, dict) else ""), 300)
recs = b.records(tasks.get("tasks") if isinstance(tasks, dict) and "tasks" in tasks else tasks)
print("task count", len(recs))
first = recs[0] if recs else {}
show("task keys", sorted(first.keys()), 1000)
show("task sample", first, 3000)
types = Counter()
for t in recs:
    for o in t.get("objectives") or []:
        types[o.get("type")] += 1
print("objective types", dict(types))
seen = set()
for t in recs:
    for o in t.get("objectives") or []:
        ty = o.get("type")
        if ty in seen:
            continue
        seen.add(ty)
        show(f"objective {ty} (task {t.get('name')})", o, 1500)
gs = next((t for t in recs if "Gunsmith" in str(t.get("name"))), None)
if gs:
    show("gunsmith task", gs, 4000)
show("rewards sample", {k: first.get(k) for k in ("startRewards", "finishRewards", "failureRewards", "experience")}, 1500)

items = b.fetch_translated("pve", "items")
its = b.records(items.get("items"))
ammo = next((i for i in its if "ammo" in (i.get("types") or []) or i.get("properties", {}).get("__typename") == "ItemPropertiesAmmo"), None)
show("ammo item", ammo, 2500)
key = next((i for i in its if "keys" in (i.get("types") or [])), None)
show("key item", key, 2000)
show("item keys", sorted(its[0].keys()) if its else None, 1000)
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
