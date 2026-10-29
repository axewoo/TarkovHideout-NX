"""Database for the app's Database tab: items, ammo, keys and quests (used by build_data.py --assets).

Everything is resolved to display strings here so the app only renders. Map
positions (quest zones, quest item spots, key locks) are converted to image
coordinates with the same projection as the Maps tab. Quest guides come from
the Escape from Tarkov wiki's plain-text extracts (CC BY-SA), fetched in
batches and stored in wiki.json.
"""

import json
import re
import sys
import time
import urllib.parse

import build_assets as a
import build_data as b

WIKI_API = "https://escapefromtarkov.fandom.com/api.php"
WIKI_SKIP = {"objectives", "rewards", "gallery", "references", "video guides", "videos", "see also"}
WIKI_MAX = 9000
DESC_MAX = 220
PINS_MAX = 16

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
        tasks = sorted((self.task(t) for t in self.tasks), key=lambda t: (t["tr"], t["lvl"], t["n"]))
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
            if "preset" in (it.get("types") or []):
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
        return {"version": 1, "mode": "pvp" if self.mode == "regular" else self.mode, "items": items,
                "ammo": ammo, "keys": keys, "tasks": tasks}


def wiki_title(link):
    if not link or "/wiki/" not in link:
        return ""
    return urllib.parse.unquote(link.split("/wiki/", 1)[1]).replace("_", " ")


def clean_extract(text):
    """Keeps the intro and the guide-like sections, marking headings with '## '."""
    parts = re.split(r"\n*^(={2,4})\s*(.+?)\s*\1\s*$\n*", text or "", flags=re.M)
    out = [parts[0].strip()]
    skip = False
    for i in range(1, len(parts) - 2, 3):
        level, title, body = len(parts[i]), parts[i + 1].strip(), parts[i + 2].strip()
        if level == 2:
            skip = title.lower() in WIKI_SKIP
        if skip:
            continue
        if body or level == 2:
            out.append(("## " if level == 2 else "### ") + title)
        if body:
            out.append(body)
    # Drop headings whose section turned out empty.
    out = [p for i, p in enumerate(out)
           if not (p.startswith("## ") and (i + 1 == len(out) or out[i + 1].startswith("## ")))]
    text = "\n\n".join(p for p in out if p)
    text = re.sub(r"\n{3,}", "\n\n", text)
    if len(text) > WIKI_MAX:
        text = text[:WIKI_MAX].rsplit("\n", 1)[0] + "\n\n(Continued on the wiki.)"
    return text


def fetch_wiki(titles):
    """Returns {title: cleaned text} for wiki pages, 20 per request."""
    titles = sorted(set(t for t in titles if t))
    out = {}
    for i in range(0, len(titles), 20):
        batch = titles[i:i + 20]
        q = urllib.parse.urlencode({"action": "query", "prop": "extracts", "explaintext": 1, "exlimit": 20,
                                    "redirects": 1, "format": "json", "titles": "|".join(batch)})
        try:
            data = json.loads(a.fetch_bytes(f"{WIKI_API}?{q}"))
        except Exception as e:  # noqa: BLE001 - the wiki is optional
            print(f"  wiki batch {i // 20} failed: {e}", file=sys.stderr)
            continue
        query = data.get("query") or {}
        alias = {}
        for kind in ("normalized", "redirects"):
            for r in query.get(kind) or []:
                alias[r.get("to")] = alias.get(r.get("from"), r.get("from"))
        for page in (query.get("pages") or {}).values():
            title = page.get("title")
            text = clean_extract(page.get("extract"))
            if not text:
                continue
            out[title] = text
            src = title
            while src in alias:
                src = alias[src]
                out[src] = text
        time.sleep(0.5)
    print(f"wiki: {len(out)} pages for {len(titles)} titles")
    return out


def build(mode, maps_loaded):
    traders = b.lookup(b.fetch_translated(mode, "traders"))
    items_data = b.fetch_translated(mode, "items") or {}
    tasks_data = b.fetch_translated(mode, "tasks") or {}
    db = Db(mode, items_data, tasks_data, traders, maps_loaded)
    return db.build(), db.links
