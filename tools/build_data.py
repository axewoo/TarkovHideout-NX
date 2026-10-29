#!/usr/bin/env python3
"""Build the compact hideout data files the Switch app reads.

json.tarkov.dev publishes full game data (items alone is tens of MB), which is
too heavy to download and parse on the Switch. This script pulls the hideout,
crafts, items and traders endpoints, resolves every id to an English name and
writes one small JSON file per game mode.

Usage: build_data.py OUT_DIR [--modes pve,regular]
"""

import argparse
import datetime
import gzip
import json
import os
import re
import sys
import time
import urllib.request

BASE = "https://json.tarkov.dev"
USER_AGENT = "TarkovHideout-NX-data/1.0 (+https://github.com/axewoo/TarkovHideout-NX)"
LANG = "en"


def fetch_json(path):
    url = f"{BASE}/{path}"
    last = None
    for attempt in range(4):
        try:
            req = urllib.request.Request(url, headers={
                "User-Agent": USER_AGENT,
                "Accept": "application/json",
                "Accept-Encoding": "gzip",
            })
            with urllib.request.urlopen(req, timeout=60) as resp:
                raw = resp.read()
                if resp.headers.get("Content-Encoding") == "gzip":
                    raw = gzip.decompress(raw)
            return json.loads(raw)
        except Exception as e:  # noqa: BLE001 - retry anything network related
            last = e
            time.sleep(2 ** attempt)
    raise RuntimeError(f"failed to fetch {url}: {last}")


# json.tarkov.dev marks translatable strings with a small JSONPath subset:
# `.name`, `.*`, `[*]`, `['a','b']` and `..name`.
_TOKEN = re.compile(
    r"^(?:\.\.([A-Za-z0-9_-]+)|\.([A-Za-z0-9_-]+)|\[('[A-Za-z0-9_-]+'(?:,'[A-Za-z0-9_-]+')*)\]|\.\*|\[\*\])"
)


def parse_path(path):
    if not path.startswith("$"):
        return None
    rest = path[1:]
    segs = []
    while rest:
        m = _TOKEN.match(rest)
        if not m:
            return None
        if m.group(1):
            segs.append(("desc", [m.group(1)]))
        elif m.group(2):
            segs.append(("child", [m.group(2)]))
        elif m.group(3):
            segs.append(("child", [n[1:-1] for n in m.group(3).split(",")]))
        else:
            segs.append(("all", None))
        rest = rest[len(m.group(0)):]
    return segs


def _children(node):
    if isinstance(node, dict):
        return list(node.keys())
    if isinstance(node, list):
        return list(range(len(node)))
    return []


def apply_path(node, segs, i, tr):
    if i == len(segs):
        if isinstance(node, str):
            return tr.get(node, node)
        return node
    if not isinstance(node, (dict, list)):
        return node
    kind, names = segs[i]
    if kind == "all":
        for k in _children(node):
            node[k] = apply_path(node[k], segs, i + 1, tr)
        return node
    if isinstance(node, dict):
        for n in names:
            if n in node:
                node[n] = apply_path(node[n], segs, i + 1, tr)
    if kind == "desc":
        for k in _children(node):
            node[k] = apply_path(node[k], segs, i, tr)
    return node


def fetch_translated(mode, endpoint):
    base = fetch_json(f"{mode}/{endpoint}")
    paths = base.get("translations") or []
    if paths:
        tr = fetch_json(f"{mode}/{endpoint}_{LANG}").get("data") or {}
        for p in paths:
            segs = parse_path(p)
            if segs is None:
                print(f"  skip unsupported translation path {p}", file=sys.stderr)
                continue
            apply_path(base, segs, 0, tr)
    return base.get("data")


def records(value):
    if isinstance(value, dict):
        return [v for v in value.values() if isinstance(v, dict)]
    if isinstance(value, list):
        return [v for v in value if isinstance(v, dict)]
    return []


def lookup(value):
    return {r["id"]: r for r in records(value) if isinstance(r.get("id"), str)}


def ref_id(value):
    if isinstance(value, str):
        return value
    if isinstance(value, dict):
        return value.get("id")
    return None


class Resolver:
    def __init__(self, items, traders, stations):
        self.items = items
        self.traders = traders
        self.stations = stations
        self.missing = set()
        self.used = {}  # item id -> (iconLink, backgroundColor)

    def item(self, value):
        iid = ref_id(value)
        raw = value if isinstance(value, dict) and "name" in value else self.items.get(iid)
        if raw is None:
            self.missing.add(iid)
            return iid or "?", iid or "?", iid or ""
        name = raw.get("name") or iid
        self.used[iid] = (raw.get("iconLink"), raw.get("backgroundColor") or "default")
        return name, raw.get("shortName") or name, iid

    def trader(self, value):
        raw = value if isinstance(value, dict) and "name" in value else self.traders.get(ref_id(value))
        return (raw or {}).get("name") or str(ref_id(value))

    def station(self, value):
        raw = value if isinstance(value, dict) and "name" in value else self.stations.get(ref_id(value))
        return (raw or {}).get("name") or str(ref_id(value))


def count_of(req):
    for k in ("count", "quantity"):
        if isinstance(req.get(k), (int, float)):
            return int(req[k])
    return 1


def is_fir(req):
    attrs = req.get("attributes")
    if isinstance(attrs, dict):
        return bool(attrs.get("foundInRaid"))
    if isinstance(attrs, list):
        for a in attrs:
            if isinstance(a, dict) and a.get("name") in ("foundInRaid", "found_in_raid"):
                return str(a.get("value")).lower() == "true"
    return bool(req.get("foundInRaid"))


def craft_items(res, entries):
    out = []
    for e in entries or []:
        if not isinstance(e, dict):
            continue
        name, short, iid = res.item(e.get("item"))
        attrs = e.get("attributes")
        if isinstance(attrs, dict) and attrs.get("tool"):
            short += " (tool)"
        out.append({"i": iid, "n": name, "s": short, "c": count_of(e)})
    return out


def build_mode(mode):
    print(f"[{mode}] fetching")
    hideout = fetch_translated(mode, "hideout")
    items_data = fetch_translated(mode, "items") or {}
    traders = lookup(fetch_translated(mode, "traders"))
    try:
        crafts_data = fetch_json(f"{mode}/crafts").get("data")
    except RuntimeError as e:
        print(f"  crafts endpoint unavailable: {e}", file=sys.stderr)
        crafts_data = None

    items = lookup(items_data.get("items") if isinstance(items_data, dict) else items_data)
    station_lookup = lookup(hideout)
    res = Resolver(items, traders, station_lookup)

    stations = []
    crafts = {}
    for st in records(hideout):
        levels = []
        for lv in sorted(records(st.get("levels")), key=lambda l: l.get("level", 0)):
            reqs = []
            for r in lv.get("itemRequirements") or []:
                if not isinstance(r, dict):
                    continue
                name, short, iid = res.item(r.get("item"))
                reqs.append({
                    "id": r.get("id") or f"{lv.get('id')}-{iid}",
                    "item": iid,
                    "n": name,
                    "s": short,
                    "c": count_of(r),
                    "fir": is_fir(r),
                })
            other = []
            for r in lv.get("stationLevelRequirements") or []:
                if isinstance(r, dict):
                    other.append(f"{res.station(r.get('station'))} level {r.get('level', '?')}")
            for r in lv.get("traderRequirements") or []:
                if isinstance(r, dict):
                    lvl = r.get("value", r.get("level", "?"))
                    other.append(f"{res.trader(r.get('trader'))} loyalty {lvl}")
            for r in lv.get("skillRequirements") or []:
                if isinstance(r, dict):
                    skill = r.get("skill")
                    sname = skill.get("name") if isinstance(skill, dict) else (skill or r.get("name") or "Skill")
                    other.append(f"{sname} {r.get('level', '?')}")
            for c in lv.get("crafts") or []:
                if isinstance(c, dict) and c.get("id"):
                    crafts[c["id"]] = (st.get("id"), lv.get("level", 0), c)
            levels.append({
                "id": lv.get("id"),
                "lvl": lv.get("level", 0),
                "time": int(lv.get("constructionTime") or 0),
                "items": reqs,
                "other": other,
            })
        stations.append({"id": st.get("id"), "name": st.get("name") or st.get("id"), "levels": levels})
    stations.sort(key=lambda s: s["name"])

    for c in records(crafts_data):
        if not c.get("id"):
            continue
        sid = ref_id(c.get("station") or c.get("hideoutStation"))
        crafts.setdefault(c["id"], (sid, c.get("level", 0), c))

    craft_out = []
    for cid, (sid, level, c) in crafts.items():
        craft_out.append({
            "id": cid,
            "st": sid,
            "lvl": int(level or c.get("level") or 0),
            "dur": int(c.get("duration") or 0),
            "req": craft_items(res, c.get("requiredItems")),
            # json.tarkov.dev has a single productItem; older shapes used rewardItems.
            "rew": craft_items(res, c.get("rewardItems") or ([c["productItem"]] if c.get("productItem") else [])),
        })
    craft_out.sort(key=lambda c: (res.station(c["st"]), c["lvl"], c["rew"][0]["n"] if c["rew"] else ""))

    skills = {}
    levels_exp = []
    if isinstance(items_data, dict):
        for sk in records(items_data.get("skills")):
            if sk.get("id"):
                skills[sk["id"]] = sk.get("name") or sk["id"]
        levels_exp = [int(l.get("exp") or 0) for l in sorted(records(items_data.get("playerLevels")),
                                                             key=lambda l: l.get("level", 0))]

    n_levels = sum(len(s["levels"]) for s in stations)
    print(f"[{mode}] {len(stations)} stations, {n_levels} levels, {len(craft_out)} crafts, "
          f"{len(items)} items in catalog, {len(res.missing)} unresolved item ids")
    if not stations or not n_levels:
        raise RuntimeError(f"[{mode}] no hideout data, refusing to write")
    return {
        "version": 1,
        "mode": "pvp" if mode == "regular" else mode,
        "generated": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d %H:%M UTC"),
        "stations": stations,
        "crafts": craft_out,
        "items": {iid: {"bg": bg} for iid, (link, bg) in sorted(res.used.items())},
        "skills": skills,
        "levelExp": levels_exp,
    }, res.used


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--modes", default="pve,regular")
    ap.add_argument("--assets", action="store_true", help="also build the icon atlas and map images")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    built = []
    used = {}
    for mode in args.modes.split(","):
        data, mode_used = build_mode(mode)
        used.update(mode_used)
        built.append((mode, data))
    dbs = []
    if args.assets:
        import build_assets  # needs Pillow and cairosvg, only installed in the data workflow
        import build_db
        links = {iid: link for iid, (link, _) in used.items()}
        for mode, _ in built:
            loaded = build_assets.load_maps(mode)
            build_assets.build_maps(mode, args.out, loaded)
            db, db_links = build_db.build(mode, loaded)
            links.update(db_links)
            dbs.append((mode, db))
        atlas = build_assets.build_icons(links, args.out)
        for _, data in built:
            for iid, meta in data["items"].items():
                if iid in atlas:
                    meta["ic"] = atlas[iid]
        titles = set()
        for _, db in dbs:
            for iid, rec in db["items"].items():
                if iid in atlas:
                    rec["ic"] = atlas[iid]
            titles.update(t["wiki"] for t in db["tasks"])
        wiki = build_db.fetch_wiki(titles, args.out)
        with open(os.path.join(args.out, "wiki.json"), "w", encoding="utf-8") as f:
            json.dump({"source": "Escape from Tarkov Wiki (escapefromtarkov.fandom.com), CC BY-SA 3.0",
                       "pages": wiki}, f, ensure_ascii=False, separators=(",", ":"))
    for mode, data in built:
        name = "pvp" if mode == "regular" else mode
        path = os.path.join(args.out, f"hideout-{name}.json")
        with open(path, "w", encoding="utf-8") as f:
            json.dump(data, f, ensure_ascii=False, separators=(",", ":"))
        print(f"wrote {path} ({os.path.getsize(path) // 1024} KB)")
    for mode, db in dbs:
        name = "pvp" if mode == "regular" else mode
        path = os.path.join(args.out, f"db-{name}.json")
        with open(path, "w", encoding="utf-8") as f:
            json.dump(db, f, ensure_ascii=False, separators=(",", ":"))
        print(f"wrote {path} ({os.path.getsize(path) // 1024} KB)")


if __name__ == "__main__":
    main()
