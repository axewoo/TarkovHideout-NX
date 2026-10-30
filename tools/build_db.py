"""Database for the app's Database tab: items, ammo, keys and quests (used by build_data.py --assets).

Everything is resolved to display strings here so the app only renders. Map
positions (quest zones, quest item spots, key locks) are converted to image
coordinates with the same projection as the Maps tab. Quest guides come from
the Escape from Tarkov wiki's plain-text extracts (CC BY-SA), fetched in
batches and stored in wiki.json.
"""

import hashlib
import io
import json
import os
import re
import sys
import urllib.parse
from html.parser import HTMLParser

from PIL import Image

import build_assets as a
import build_data as b

WIKI_API = "https://escapefromtarkov.fandom.com/api.php"
WIKI_SKIP = {"objectives", "rewards", "gallery", "references", "video guides", "videos", "see also"}
WIKI_MAX = 9000
WIKI_IMG_W = 560      # guide images are stored at most this wide
WIKI_IMG_MIN_W = 150  # smaller pictures are item icons
WIKI_IMG_MAX = 12     # per page
DESC_MAX = 220
PINS_MAX = 16

TRADER_ORDER = ["Prapor", "Therapist", "Fence", "Skier", "Peacekeeper", "Mechanic", "Ragman", "Jaeger", "Ref",
                "Lightkeeper", "BTR Driver"]
SHORT_CALIBERS = {"46", "57", "68", "86", "93"}
BUILD_ATTRS = [
    ("ergonomics", "Ergonomics"), ("recoil", "Recoil sum"), ("weight", "Weight"), ("durability", "Durability"),
    ("magazineCapacity", "Magazine"), ("effectiveDistance", "Sighting range"), ("accuracy", "Accuracy"),
    ("muzzleVelocity", "Muzzle velocity"), ("width", "Width"), ("height", "Height"),
]


def caliber_name(raw):
    s = (raw or "").replace("Caliber", "")
    m = re.match(r"(\d+)(x.*)?$", s)
    if not m:
        return s
    num, rest = m.group(1), m.group(2) or ""
    if len(num) == 3 and num[0] == "1":
        num = f"{num[:2]}.{num[2]}"
    elif len(num) == 3:
        num = f"{num[0]}.{num[1:]}"
    elif len(num) == 4:
        num = f"{num[:2]}.{num[2:]}"
    elif num in SHORT_CALIBERS:
        num = f"{num[0]}.{num[1]}"
    return num + rest


def short_text(s, n):
    s = re.sub(r"\s+", " ", s or "").strip()
    return s if len(s) <= n else s[:n - 1].rsplit(" ", 1)[0] + "..."


def best_sell(item, traders):
    best = None
    for o in item.get("sellToTrader") or []:
        price = o.get("priceRUB") or o.get("price") or 0
        if best is None or price > best[1]:
            best = (traders.get(o.get("trader"), {}).get("name") or "Trader", int(price))
    return best


class Db:
    def __init__(self, mode, items_data, tasks_data, traders, maps_loaded):
        self.mode = mode
        self.items = b.lookup(items_data.get("items"))
        self.handbook = items_data.get("handbookCategories") or {}
        self.categories = items_data.get("itemCategories") or {}
        self.skills = {s.get("id"): s.get("name") for s in b.records(items_data.get("skills"))}
        self.traders = traders
        self.quest_items = b.lookup(tasks_data.get("questItems")) if isinstance(tasks_data, dict) else {}
        self.tasks = b.records(tasks_data.get("tasks") if isinstance(tasks_data, dict) else tasks_data)
        self.task_names = {x.get("id"): x.get("name") for x in self.tasks}
        _, game_maps, meta = maps_loaded
        self.game_maps = game_maps
        self.map_names = {m.get("id"): m.get("name") for m in game_maps}
        self.proj = a.MapProjector(game_maps, meta)
        self.used = set()    # item ids that get an icon
        self.links = {}      # item id -> icon url

    def ref(self, iid):
        iid = b.ref_id(iid)
        if not iid:
            return None
        rec = self.items.get(iid) or self.quest_items.get(iid)
        if rec:
            self.used.add(iid)
            self.links[iid] = rec.get("iconLink")
        return iid

    def pins(self, zones=(), locations=()):
        out = []
        for z in zones or []:
            p = self.proj.uv(b.ref_id(z.get("map")), z.get("position"))
            if p:
                out.append(list(p))
        for loc in locations or []:
            for pos in loc.get("positions") or []:
                p = self.proj.uv(b.ref_id(loc.get("map")), pos)
                if p:
                    out.append(list(p))
        seen, uniq = set(), []
        for p in out:
            k = (p[0], round(p[1], 3), round(p[2], 3))
            if k not in seen:
                seen.add(k)
                uniq.append(p)
        return uniq[:PINS_MAX]

    def trader_name(self, tid):
        return self.traders.get(b.ref_id(tid), {}).get("name") or "?"

    def objective(self, o):
        out = {"d": o.get("description") or "", "t": o.get("type") or "", "o": bool(o.get("optional"))}
        if isinstance(o.get("count"), (int, float)) and o.get("count") > 1:
            out["c"] = int(o["count"])
        ids = []
        for key in ("items", "useAny"):
            ids += [self.ref(i) for i in (o.get(key) or [])[:8]]
        for key in ("questItem", "markerItem"):
            if o.get(key):
                ids.append(self.ref(o[key]))
        ids = [i for i in ids if i]
        if ids:
            out["it"] = ids
            if len(o.get("items") or []) > 8:
                out["more"] = len(o["items"]) - 8
        if o.get("foundInRaid"):
            out["fir"] = True
        pins = self.pins(o.get("zones"), o.get("possibleLocations"))
        if pins:
            out["pins"] = pins
        if o.get("type") == "buildWeapon":
            gs = {"w": self.ref(o.get("item")), "attrs": [], "parts": [], "cats": []}
            attrs = o.get("buildAttributes") or {}
            for key, label in BUILD_ATTRS:
                at = attrs.get(key) or {}
                val, cmp = at.get("value"), at.get("compareMethod") or ">="
                if val in (None, 0) and cmp == ">=":
                    continue
                gs["attrs"].append([label, cmp, val])
            gs["parts"] = [p for p in (self.ref(i) for i in o.get("containsAll") or []) if p]
            for c in o.get("containsCategory") or []:
                cat = self.categories.get(b.ref_id(c)) if isinstance(self.categories, dict) else None
                gs["cats"].append((cat or {}).get("name") or str(b.ref_id(c)))
            out["gs"] = gs
            if gs["w"]:
                out.setdefault("it", []).insert(0, gs["w"])
        if o.get("type") == "skill" and o.get("skill"):
            out["d"] = out["d"] or f"Reach {o['skill']} level {o.get('level')}"
        return out

    def rewards(self, r):
        r = r or {}
        out = {}
        rep = [[self.trader_name(t.get("trader")), round(t.get("standing") or 0, 2)]
               for t in r.get("traderStanding") or []]
        if rep:
            out["rep"] = rep
        items = [[self.ref(i.get("item")), int(i.get("count") or 1)] for i in r.get("items") or []]
        items = [i for i in items if i[0]]
        if items:
            out["it"] = items
        unl = []
        for u in r.get("offerUnlock") or []:
            iid = self.ref(u.get("item"))
            if iid:
                unl.append([iid, self.trader_name(u.get("trader")), u.get("level") or 1])
        if unl:
            out["un"] = unl
        sk = [[self.skills.get(s.get("name")) or s.get("name") or "?", s.get("level")]
              for s in r.get("skillLevelReward") or []]
        if sk:
            out["sk"] = sk
        tu = [self.trader_name(t) for t in r.get("traderUnlock") or []]
        if tu:
            out["tu"] = tu
        return out

    def task(self, t):
        out = {
            "id": t.get("id"),
            "n": t.get("name") or "?",
            "tr": self.trader_name(t.get("trader")),
            "lvl": t.get("minPlayerLevel") or 0,
            "map": self.map_names.get(b.ref_id(t.get("map")), ""),
            "exp": t.get("experience") or 0,
        }
        if t.get("kappaRequired"):
            out["k"] = True
        if t.get("lightkeeperRequired"):
            out["lk"] = True
        if t.get("factionName") and t["factionName"] != "Any":
            out["fac"] = t["factionName"]
        pre = [self.task_names.get(b.ref_id(r.get("task"))) for r in t.get("taskRequirements") or []]
        pre = [p for p in pre if p]
        if pre:
            out["pre"] = pre
        trl = [f"{self.trader_name(r.get('trader'))} LL{r.get('value')}" for r in t.get("traderRequirements") or []
               if r.get("requirementType") == "level"]
        if trl:
            out["trl"] = trl
        out["obj"] = [self.objective(o) for o in t.get("objectives") or []]
        keys = []
        for k in t.get("neededKeys") or []:
            for kid in (k.get("keys") if isinstance(k, dict) else [k]) or []:
                kid = self.ref(kid)
                if kid and kid not in keys:
                    keys.append(kid)
        if keys:
            out["keys"] = keys
        rw = self.rewards(t.get("finishRewards"))
        if rw:
            out["rw"] = rw
        st = self.rewards(t.get("startRewards")).get("it")
        if st:
            out["st"] = st
        out["wiki"] = wiki_title(t.get("wikiLink"))
        return out

    def build(self):
        def order(t):
            rank = TRADER_ORDER.index(t["tr"]) if t["tr"] in TRADER_ORDER else len(TRADER_ORDER)
            return rank, t["tr"], t["lvl"], t["n"]
        tasks = sorted((self.task(t) for t in self.tasks), key=order)
        apply_quest_groups(tasks)
        key_quests = {}
        for t in tasks:
            for k in t.get("keys", []):
                key_quests.setdefault(k, []).append(t["n"])

        ammo, keys = [], []
        for iid, it in self.items.items():
            types = it.get("types") or []
            props = it.get("properties") or {}
            if "ammo" in types and props.get("propertiesType") == "ItemPropertiesAmmo":
                self.ref(iid)
                ammo.append({
                    "i": iid, "cal": caliber_name(props.get("caliber")),
                    "dmg": props.get("damage") or 0, "pen": props.get("penetrationPower") or 0,
                    "ad": props.get("armorDamage") or 0,
                    "fr": round(100 * (props.get("fragmentationChance") or 0)),
                    "v": props.get("initialSpeed") or 0, "pc": props.get("projectileCount") or 1,
                    "tr": bool(props.get("tracer")),
                })
            elif "keys" in types:
                self.ref(iid)
                rec = {"i": iid, "u": props.get("uses") or 0}
                if iid in key_quests:
                    rec["q"] = key_quests[iid]
                keys.append(rec)
        ammo.sort(key=lambda x: (x["cal"], -x["pen"]))
        lock_pins = {}
        for gm in self.game_maps:
            for lock in gm.get("locks") or []:
                kid = b.ref_id(lock.get("key"))
                p = self.proj.uv(gm.get("id"), lock.get("position"))
                if kid and p:
                    lock_pins.setdefault(kid, []).append(list(p))
        for k in keys:
            if k["i"] in lock_pins:
                k["pins"] = lock_pins[k["i"]][:PINS_MAX]
        keys.sort(key=lambda k: self.items[k["i"]].get("name") or "")

        items = {}
        for iid, it in self.items.items():
            # Presets only matter where a quest gives or asks for one (weapon rewards).
            if "preset" in (it.get("types") or []) and iid not in self.used:
                continue
            hb = (it.get("handbookCategories") or [None])[0]
            cat = (self.handbook.get(b.ref_id(hb)) or {}).get("name") if isinstance(self.handbook, dict) else None
            rec = {"n": it.get("name") or iid, "s": it.get("shortName") or "", "c": cat or ""}
            price = it.get("avg24hPrice") or it.get("lastLowPrice")
            if price:
                rec["p"] = int(price)
            sell = best_sell(it, self.traders)
            if sell:
                rec["t"] = list(sell)
            if it.get("weight"):
                rec["w"] = round(it["weight"], 3)
            if "noFlea" in (it.get("types") or []):
                rec["nf"] = True
            desc = short_text(it.get("description"), DESC_MAX)
            if desc:
                rec["d"] = desc
            items[iid] = rec
        for iid, q in self.quest_items.items():
            items[iid] = {"n": q.get("name") or iid, "s": q.get("shortName") or "", "c": "Quest item", "q": True}

        print(f"[{self.mode}] database: {len(tasks)} quests, {len(ammo)} ammo, {len(keys)} keys, {len(items)} items, "
              f"{sum(1 for k in keys if 'pins' in k)} keys with lock positions, "
              f"{sum(1 for t in tasks for o in t['obj'] if 'pins' in o)} objectives with map pins")
        # Traders that give quests, in the game's order, with their portrait (saved by save_trader_icons).
        givers = {t["tr"] for t in tasks}
        traders = []
        for tr in sorted(self.traders.values(), key=lambda r: (TRADER_ORDER.index(r.get("name"))
                                                               if r.get("name") in TRADER_ORDER else 99,
                                                               r.get("name") or "")):
            if tr.get("name") not in givers:
                continue
            slug = tr.get("normalizedName") or re.sub(r"[^a-z0-9]+", "-", tr["name"].lower())
            traders.append({"n": tr["name"], "img": f"traders/{slug}.jpg", "src": tr.get("imageLink") or ""})
        guns = self.guns()
        return {"version": 1, "mode": "pvp" if self.mode == "regular" else self.mode, "items": items,
                "ammo": ammo, "keys": keys, "tasks": tasks, "traders": traders, "guns": guns}

    def buy_offers(self, it):
        """Trader offers as [trader, loyalty level, price in roubles, unlocking quest or ""], cheapest first."""
        out = []
        for o in it.get("buyFromTrader") or []:
            price = o.get("priceRUB") or o.get("price") or 0
            quest = self.task_names.get(b.ref_id(o.get("taskUnlock")), "") if o.get("taskUnlock") else ""
            out.append([self.trader_name(o.get("trader")), o.get("minTraderLevel") or 1, int(price), quest or ""])
        out.sort(key=lambda o: o[2])
        return out[:3]

    def guns(self):
        """Weapons, the mods that fit them and their stats, for the gun builder.

        Slots list the mods they take as an index into "sets" (many slots share the same list).
        Each weapon's default build ("d") is a tree of [slot index, mod id, [child slots]].
        """
        by_cat = {}
        for iid, it in self.items.items():
            for c in it.get("categories") or []:
                by_cat.setdefault(b.ref_id(c), set()).add(iid)
        sets, set_index = [], {}
        mods, weapons = {}, []

        def allowed(slot):
            f = slot.get("filters") or {}
            ids = {b.ref_id(i) for i in f.get("allowedItems") or []}
            for c in f.get("allowedCategories") or []:
                ids |= by_cat.get(b.ref_id(c), set())
            for c in f.get("excludedCategories") or []:
                ids -= by_cat.get(b.ref_id(c), set())
            ids -= {b.ref_id(i) for i in f.get("excludedItems") or []}
            return sorted(i for i in ids if i in self.items and "preset" not in (self.items[i].get("types") or []))

        def slots_of(it):
            out = []
            for sl in (it.get("properties") or {}).get("slots") or []:
                ids = allowed(sl)
                if not ids:
                    continue
                key = tuple(ids)
                if key not in set_index:
                    set_index[key] = len(sets)
                    sets.append(list(ids))
                out.append([sl.get("name") or sl.get("nameId") or "Slot", 1 if sl.get("required") else 0,
                            set_index[key], sl.get("id") or ""])
                for i in ids:
                    queue.append(i)
            return out

        queue = []

        def add_mod(iid):
            if iid in mods:
                return
            it = self.items[iid]
            props = it.get("properties") or {}
            m = {}
            for key, src in (("e", "ergonomicsModifier"), ("r", "recoilModifier"), ("a", "accuracyModifier"),
                             ("v", "velocity")):
                val = it.get(src)
                if val is None and src == "ergonomicsModifier":
                    val = props.get("ergonomics")
                if val:
                    m[key] = round(val, 2)
            if props.get("capacity"):
                m["cap"] = props["capacity"]
            if props.get("sightingRange"):
                m["sr"] = props["sightingRange"]
            if props.get("zoomLevels"):
                zooms = sorted({z for zl in props["zoomLevels"] for z in (zl or []) if z})
                if zooms:
                    m["zm"] = [zooms[0], zooms[-1]]
            conflicts = [b.ref_id(c) for c in it.get("conflictingItems") or []]
            if conflicts:
                m["cf"] = [c for c in conflicts if c]
            offers = self.buy_offers(it)
            if offers:
                m["buy"] = offers
            mods[iid] = m
            m["sl"] = slots_of(it)
            if not m["sl"]:
                del m["sl"]
            self.ref(iid)

        presets = {}
        for iid, it in self.items.items():
            props = it.get("properties") or {}
            if props.get("propertiesType") == "ItemPropertiesPreset" and props.get("default"):
                presets[b.ref_id(props.get("baseItem"))] = it

        def place(slots, parts, depth=0):
            """Fits the preset's flat part list into the slot tree, first fitting part per slot."""
            tree = []
            for si, sl in enumerate(slots):
                allow = set(sets[sl[2]])
                for pi, pid in enumerate(parts):
                    if pid in allow:
                        parts.pop(pi)
                        child = place(mods[pid].get("sl", []), parts, depth + 1) if depth < 12 else []
                        tree.append([si, pid, child])
                        break
            return tree

        for iid, it in self.items.items():
            props = it.get("properties") or {}
            if props.get("propertiesType") != "ItemPropertiesWeapon":
                continue
            w = {"i": iid, "cal": caliber_name(props.get("caliber")), "e": props.get("ergonomics") or 0,
                 "rv": props.get("recoilVertical") or 0, "rh": props.get("recoilHorizontal") or 0,
                 "fr": props.get("fireRate") or 0, "sr": props.get("sightingRange") or 0,
                 "img": f"guns/{iid}.jpg", "src": it.get("image512pxLink") or it.get("inspectImageLink") or ""}
            offers = self.buy_offers(it)
            if offers:
                w["buy"] = offers
            w["sl"] = slots_of(it)
            while queue:
                mid = queue.pop()
                if mid not in mods:
                    add_mod(mid)
            self.ref(iid)
            pre = presets.get(iid)
            if pre:
                parts = [b.ref_id(c.get("item")) for c in pre.get("containsItems") or []]
                parts = [p for p in parts if p and p != iid and p in mods]
                w["d"] = place(w["sl"], parts)
                pp = pre.get("properties") or {}
                w["pre"] = [pp.get("ergonomics") or 0, pp.get("recoilVertical") or 0, pp.get("recoilHorizontal") or 0]
            weapons.append(w)
        weapons.sort(key=lambda w: (self.items[w["i"]].get("name") or ""))

        # Checks the stat formulas the app uses against the game's numbers for each default build.
        def walk(tree):
            for _, mid, kids in tree:
                yield mid
                yield from walk(kids)
        off = []
        for w in weapons:
            if "pre" not in w:
                continue
            ms = [mods[m] for m in walk(w["d"])]
            erg = w["e"] + sum(m.get("e", 0) for m in ms)
            rv = w["rv"] * (1 + sum(m.get("r", 0) for m in ms) / 100)
            if abs(erg - w["pre"][0]) > 1 or abs(rv - w["pre"][1]) > 1.5:
                off.append((self.items[w["i"]].get("shortName"), round(erg, 1), w["pre"][0], round(rv, 1), w["pre"][1]))
        print(f"[{self.mode}] guns: {len(weapons)} weapons, {len(mods)} mods, {len(sets)} slot lists; "
              f"{len(off)} default builds off the game's stats: {off[:12]}")
        return {"w": weapons, "m": mods, "sets": sets}


QUEST_GROUPS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "quest_groups.json")


def apply_quest_groups(tasks):
    """Sorts quests into the game's sections: "g" is the loyalty level section (1-4), 0 for Essential
    tasks, -1 for traders without sections (Lightkeeper, BTR Driver); "go" orders them like the game
    and "gh" names the Essential quest a finished chain folds under. Quests the screenshots don't show
    follow the quest before them, else the loyalty level tarkov.dev gives."""
    try:
        with open(QUEST_GROUPS, encoding="utf-8") as f:
            groups = json.load(f)
    except (OSError, ValueError):
        return
    norm = lambda n: (n or "").replace("\u2019", "'").strip().lower()
    known = {}
    for trader, sec in groups.items():
        if not isinstance(sec, dict):
            continue
        for lv in range(1, 5):
            for i, n in enumerate(sec.get(f"LL{lv}", [])):
                known[(trader, norm(n))] = (lv, lv * 1000 + i, "")
        for ci, chain in enumerate(sec.get("essential", [])):
            for i, n in enumerate(chain):
                known[(trader, norm(n))] = (0, 5000 + ci * 100 + i, chain[0] if i else "")
    by_name = {}
    for t in tasks:
        by_name.setdefault(norm(t["n"]), []).append(t)
    placed = {}

    def place(t, depth=0):
        key = id(t)
        if key in placed:
            return placed[key]
        k = (t["tr"], norm(t["n"]))
        if t["tr"] not in groups:
            # No sections in the game: one list, earlier quests first.
            d = 0
            for p in t.get("pre", []):
                for q in by_name.get(norm(p), []):
                    if q["tr"] == t["tr"] and depth < 40:
                        d = max(d, place(q, depth + 1)[1] + 1)
            placed[key] = (-1, d, "")
        elif k in known:
            placed[key] = known[k]
        else:
            res = None
            for p in t.get("pre", []):
                for q in by_name.get(norm(p), []):
                    if q["tr"] == t["tr"] and depth < 40:
                        g, o, h = place(q, depth + 1)
                        # Follows its prerequisite. In Essential tasks it comes after that chain's block
                        # as its own entry, since the game shows the newest quest of a chain on top.
                        if g == 0:
                            base = o // 100 * 100
                            cand = (0, max(o, base + 49) + 1, "")
                        else:
                            cand = (g, o + 0.5, "")
                        if res is None or cand[1] > res[1]:
                            res = cand
            if res is None:
                lv = 1
                for r in t.get("trl", []):
                    if r.startswith(t["tr"] + " LL"):
                        lv = max(1, min(4, int(r.rsplit("LL", 1)[1] or 1)))
                res = (lv, lv * 1000 + 900, "")
            placed[key] = res
        return placed[key]

    for t in tasks:
        place(t)
    for t in tasks:
        g, o, h = placed[id(t)]
        t["g"] = g
        t["go"] = round(o * 2)
        if h:
            t["gh"] = h


TRADER_ICON_PX = 128


GUN_IMG_W = 512


def save_gun_pictures(db, out):
    """Downloads each weapon's picture once as a JPEG on the app's cell colour."""
    os.makedirs(os.path.join(out, "guns"), exist_ok=True)
    done = 0
    for w in db.get("guns", {}).get("w", []):
        src = w.pop("src", "")
        path = os.path.join(out, w["img"])
        if not os.path.exists(path) and src:
            try:
                im = Image.open(io.BytesIO(a.fetch_bytes(src))).convert("RGBA")
                im.thumbnail((GUN_IMG_W, GUN_IMG_W), Image.LANCZOS)
                bg = Image.new("RGBA", im.size, a.CELL_RGB + (255,))
                bg.alpha_composite(im)
                bg.convert("RGB").save(path, quality=86)
            except Exception as e:  # noqa: BLE001
                print(f"  gun picture {w['i']} failed: {e}", file=sys.stderr)
        if os.path.exists(path):
            done += 1
        else:
            w.pop("img")
    print(f"gun pictures: {done} of {len(db.get('guns', {}).get('w', []))}")


def save_trader_icons(db, out):
    """Downloads each trader's portrait once as a square JPEG; drops traders whose picture fails."""
    os.makedirs(os.path.join(out, "traders"), exist_ok=True)
    kept = []
    for tr in db.get("traders", []):
        src = tr.pop("src", "")
        path = os.path.join(out, tr["img"])
        if not os.path.exists(path) and src:
            print(f"  trader icon {tr['n']}: {src}")
            try:
                im = Image.open(io.BytesIO(a.fetch_bytes(src))).convert("RGBA")
                bg = Image.new("RGBA", im.size, a.CELL_RGB + (255,))
                bg.alpha_composite(im)
                bg.convert("RGB").resize((TRADER_ICON_PX, TRADER_ICON_PX), Image.LANCZOS).save(path, quality=88)
            except Exception as e:  # noqa: BLE001
                print(f"  trader icon {tr['n']} failed: {e}", file=sys.stderr)
        if not os.path.exists(path):
            tr.pop("img")
        kept.append(tr)
    db["traders"] = kept
    print(f"trader icons: {sum('img' in t for t in kept)} of {len(kept)}")


def wiki_title(link):
    if not link or "/wiki/" not in link:
        return ""
    return urllib.parse.unquote(link.split("/wiki/", 1)[1]).replace("_", " ")


class WikiImages(HTMLParser):
    """Collects guide pictures from a parsed wiki page, keyed by the heading they sit under.

    Infobox and navbox tables are skipped (banner, icons), as are pictures
    narrower than WIKI_IMG_MIN_W, which are item icons next to the text.
    """

    def __init__(self):
        super().__init__()
        self.section = ""
        self.heading = None    # text of the heading being read, None outside headings
        self.in_headline = False  # only the mw-headline span counts, not the "[edit | edit source]" links
        self.table_depth = 0
        self.images = {}  # section title -> [(key, url, caption)]
        self.seen = set()

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        if tag == "table":
            self.table_depth += 1
        elif tag in ("h2", "h3"):
            self.heading = ""
        elif tag == "span" and self.heading is not None and "mw-headline" in (a.get("class") or ""):
            self.in_headline = True
        elif tag == "img" and not self.table_depth and self.heading is None:
            src = a.get("data-src") or a.get("src") or ""
            key = a.get("data-image-key")
            try:
                width = int(a.get("width") or 0)
            except ValueError:
                width = 0
            if not key or "static.wikia.nocookie.net" not in src or width < WIKI_IMG_MIN_W or key in self.seen:
                return
            self.seen.add(key)
            base = re.sub(r"/revision/latest.*", "/revision/latest", src)
            cb = re.search(r"cb=(\d+)", src)
            url = f"{base}/scale-to-width-down/{WIKI_IMG_W}" + (f"?cb={cb.group(1)}" if cb else "")
            alt = (a.get("alt") or "").strip()
            self.images.setdefault(self.section, []).append((key, url, alt if " " in alt else ""))

    def handle_endtag(self, tag):
        if tag == "span":
            self.in_headline = False
        if tag == "table":
            self.table_depth = max(0, self.table_depth - 1)
        elif tag in ("h2", "h3") and self.heading is not None:
            self.section = re.sub(r"\s+", " ", self.heading).strip()
            self.heading = None

    def handle_data(self, data):
        if self.in_headline:
            self.heading += data


def wiki_image_lines(images, out):
    """Downloads guide pictures as JPEGs under out/wiki/ and returns the '@img' lines for them."""
    lines = []
    for key, url, caption in images:
        name = "wiki/" + hashlib.sha1(key.encode()).hexdigest()[:16] + ".jpg"
        path = os.path.join(out, name)
        try:
            if not os.path.exists(path):
                img = Image.open(io.BytesIO(a.fetch_bytes(url))).convert("RGBA")
                if img.width > WIKI_IMG_W:
                    img = img.resize((WIKI_IMG_W, round(img.height * WIKI_IMG_W / img.width)), Image.LANCZOS)
                flat = Image.new("RGB", img.size, a.CELL_RGB)
                flat.paste(img, (0, 0), img)
                os.makedirs(os.path.dirname(path), exist_ok=True)
                flat.save(path, quality=80, optimize=True)
            with Image.open(path) as im:
                w, h = im.size
        except Exception as e:  # noqa: BLE001 - pictures are optional
            print(f"  wiki image {key} failed: {e}", file=sys.stderr)
            continue
        lines.append(f"@img {name} {w} {h}")
        if caption:
            lines.append(f"@cap {caption}")
    return lines


def clean_extract(text, images=None, img_dir=None):
    """Keeps the intro and the guide-like sections, marking headings with '## '.

    With images ({section title: pictures}) and img_dir, each kept section ends
    with '@img path w h' lines (and '@cap caption'), which the app draws inline.
    The wiki's Gallery section is kept for its pictures only.
    """
    images = images or {}
    budget = [WIKI_IMG_MAX]

    def pics(title):
        if img_dir is None or not budget[0]:
            return ""
        chosen = images.get(title, [])[:budget[0]]
        budget[0] -= len(chosen)
        return "\n".join(wiki_image_lines(chosen, img_dir))

    parts = re.split(r"\n*^(={2,4})\s*(.+?)\s*\1\s*$\n*", text or "", flags=re.M)
    blocks = [parts[0].strip(), pics("")]
    skip = False
    for i in range(1, len(parts) - 2, 3):
        level, title, body = len(parts[i]), parts[i + 1].strip(), parts[i + 2].strip()
        if level == 2:
            skip = title.lower() in WIKI_SKIP
        if skip:
            if title.lower() == "gallery":
                gallery = pics(title)
                if gallery:
                    blocks += ["## Gallery", gallery]
            continue
        body = "\n\n".join(p for p in (body, pics(title)) if p)
        if body or level == 2:
            blocks.append(("## " if level == 2 else "### ") + title)
        if body:
            blocks.append(body)
    out = blocks
    # Drop headings whose section turned out empty.
    out = [p for i, p in enumerate(out)
           if not (p.startswith("## ") and (i + 1 == len(out) or out[i + 1].startswith("## ")))]
    text = "\n\n".join(p for p in out if p)
    # Table remnants leave tab-only lines; strip them before collapsing blank runs.
    text = re.sub(r"[ \t\u200e]+$", "", re.sub(r"^[ \t]+", "", text, flags=re.M), flags=re.M)
    text = re.sub(r"\n{3,}", "\n\n", text)
    if len(text) > WIKI_MAX:
        text = text[:WIKI_MAX].rsplit("\n", 1)[0] + "\n\n(Continued on the wiki.)"
    return text


def fetch_wiki_images(title):
    q = urllib.parse.urlencode({"action": "parse", "page": title, "prop": "text", "redirects": 1, "format": "json"})
    try:
        data = json.loads(a.fetch_bytes(f"{WIKI_API}?{q}"))
        parser = WikiImages()
        parser.feed(((data.get("parse") or {}).get("text") or {}).get("*") or "")
        return parser.images
    except Exception as e:  # noqa: BLE001 - pictures are optional
        print(f"  wiki images {title} failed: {e}", file=sys.stderr)
        return {}


def fetch_wiki_page(title, out=None):
    # TextExtracts only returns one whole-page extract per request.
    q = urllib.parse.urlencode({"action": "query", "prop": "extracts", "explaintext": 1, "redirects": 1,
                                "format": "json", "titles": title})
    try:
        data = json.loads(a.fetch_bytes(f"{WIKI_API}?{q}"))
    except Exception as e:  # noqa: BLE001 - the wiki is optional
        print(f"  wiki {title} failed: {e}", file=sys.stderr)
        return title, ""
    for page in ((data.get("query") or {}).get("pages") or {}).values():
        images = fetch_wiki_images(title) if out else None
        return title, clean_extract(page.get("extract"), images, out)
    return title, ""


def fetch_wiki(titles, out=None):
    """Returns {title: cleaned text} for wiki pages; with out, guide pictures are saved under out/wiki/."""
    from concurrent.futures import ThreadPoolExecutor

    titles = sorted(set(t for t in titles if t))
    with ThreadPoolExecutor(max_workers=2) as pool:
        pages = {t: text for t, text in pool.map(lambda t: fetch_wiki_page(t, out), titles) if text}
    n_img = sum(text.count("\n@img ") + text.startswith("@img ") for text in pages.values())
    print(f"wiki: {len(pages)} pages for {len(titles)} titles ({sum(map(len, pages.values())) // 1024} KB), "
          f"{n_img} pictures")
    return pages


def build(mode, maps_loaded):
    traders = b.lookup(b.fetch_translated(mode, "traders"))
    items_data = b.fetch_translated(mode, "items") or {}
    tasks_data = b.fetch_translated(mode, "tasks") or {}
    db = Db(mode, items_data, tasks_data, traders, maps_loaded)
    return db.build(), db.links
