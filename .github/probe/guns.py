#!/usr/bin/env python3
"""Temporary: prints weapon/mod data shapes for the gun builder."""
import json, sys
from collections import Counter
sys.path.insert(0, "tools")
import build_data as b

def show(label, obj, n=6000):
    print(f"--- {label}: {json.dumps(obj, ensure_ascii=False)[:n]}")

items = b.fetch_translated("pve", "items")
print("top keys", list(items.keys()))
its = b.records(items.get("items"))
print("count", len(its))
print("prop types", Counter((i.get("properties") or {}).get("propertiesType") for i in its).most_common(60))
by = {i["id"]: i for i in its}
show("M4A1", by.get("5447a9cd4bdc2dbd208b4567"), 9000)
show("AK-74N", by.get("5644bd2b4bdc2d3b4c8b4572"), 9000)
for t in ("ItemPropertiesWeaponMod", "ItemPropertiesScope", "ItemPropertiesMagazine", "ItemPropertiesBarrel", "ItemPropertiesPreset"):
    x = next((i for i in its if (i.get("properties") or {}).get("propertiesType") == t), None)
    show(t, x, 4000)
show("image keys", sorted(k for k in its[0] if "ink" in k or "mage" in k))
w = [i for i in its if (i.get("properties") or {}).get("propertiesType") == "ItemPropertiesWeapon"]
print("weapons", len(w))
mods = [i for i in its if "mods" in (i.get("types") or [])]
print("mods", len(mods))
for k, v in items.items():
    if k != "items":
        show("other " + k, v if not isinstance(v, (dict, list)) else str(v)[:800], 900)
