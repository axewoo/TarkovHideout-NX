"""Item icon atlas and map images for the Switch app (used by build_data.py --assets).

Icons: every item the hideout data references is packed into one PNG atlas of
64 px cells, so the app makes a single download and a single texture.

Maps: tarkov.dev's interactive map metadata (the-hideout/tarkov-dev maps.json)
says how game coordinates project onto each map image. Each map is rendered
once to a JPEG of at most MAP_PX on its long side, and extracts, transits and
boss spawns from json.tarkov.dev are converted to 0..1 image coordinates so
the app never needs to know about projections.
"""

import io
import json
import math
import os
import re
import sys
import time
import urllib.request

from PIL import Image

import build_data as b

ICON_PX = 64
ATLAS_COLS = 32
PAGE_ICONS = ATLAS_COLS * ATLAS_COLS
CELL_RGB = (36, 37, 32)  # col::cell in the app
MAP_PX = 4096
MAPS_META = "https://raw.githubusercontent.com/the-hideout/tarkov-dev/main/src/data/maps.json"

# Game map ids that reuse another map's image.
MAP_ALIASES = {
    "night-factory": "factory",
    "ground-zero-21": "ground-zero",
    "ground-zero-tutorial": "ground-zero",
    "the-lab-dark": "the-lab",
}
SKIP_MAPS = {"ground-zero-tutorial"}
# PMC bots show up in the boss list on PVE; they are not worth a marker.
PMC_BOTS = {"BEAR", "USEC", "PMC"}


def fetch_bytes(url):
    last = None
    for attempt in range(4):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": b.USER_AGENT})
            with urllib.request.urlopen(req, timeout=60) as resp:
                return resp.read()
        except urllib.error.HTTPError as e:
            if e.code == 404:
                raise
            last = e
        except Exception as e:  # noqa: BLE001 - retry anything network related
            last = e
        time.sleep(2 ** attempt)
    raise RuntimeError(f"failed to fetch {url}: {last}")


def build_icons(links, out):
    """Writes icons-<page>.jpg atlases and returns {item id: global icon index}.

    Each page is ATLAS_COLS x ATLAS_COLS cells, so index // PAGE_ICONS is the
    page. Icons are flattened onto the app's cell colour, which lets the pages
    be JPEGs at a fraction of the PNG size.
    """
    ids = sorted(iid for iid, link in links.items() if link)
    index = {}
    pages = []
    failed = 0
    for iid in ids:
        try:
            img = Image.open(io.BytesIO(fetch_bytes(links[iid]))).convert("RGBA")
        except Exception as e:  # noqa: BLE001
            failed += 1
            print(f"  icon {iid} failed: {e}", file=sys.stderr)
            continue
        img.thumbnail((ICON_PX, ICON_PX), Image.LANCZOS)
        n = len(index)
        page, cell = divmod(n, PAGE_ICONS)
        if page == len(pages):
            pages.append(Image.new("RGB", (ATLAS_COLS * ICON_PX, ATLAS_COLS * ICON_PX), CELL_RGB))
        x = (cell % ATLAS_COLS) * ICON_PX + (ICON_PX - img.width) // 2
        y = (cell // ATLAS_COLS) * ICON_PX + (ICON_PX - img.height) // 2
        pages[page].paste(img, (x, y), img)
        index[iid] = n
    total = 0
    for i, page in enumerate(pages):
        rows = math.ceil(min(PAGE_ICONS, len(index) - i * PAGE_ICONS) / ATLAS_COLS)
        page = page.crop((0, 0, ATLAS_COLS * ICON_PX, rows * ICON_PX))
        path = os.path.join(out, f"icons-{i}.jpg")
        page.save(path, quality=90, optimize=True)
        total += os.path.getsize(path)
    print(f"wrote {len(pages)} icon pages: {len(index)} icons, {failed} failed ({total // 1024} KB)")
    return index


# --- maps -----------------------------------------------------------------

class Projection:
    """tarkov.dev's Leaflet CRS: game (x, z) -> rotated -> affine transform."""

    def __init__(self, meta):
        self.a, self.b, self.c, self.d = (meta.get("transform") or [1, 0, 1, 0])
        rot = math.radians(meta.get("coordinateRotation") or 0)
        self.cos, self.sin = math.cos(rot), math.sin(rot)

    def point(self, x, z):
        # Leaflet latLng is (lat=z, lng=x); rotation works on (lng, lat).
        rx = x * self.cos - z * self.sin
        ry = x * self.sin + z * self.cos
        return self.a * rx + self.b, -self.c * ry + self.d

    def rect(self, bounds):
        # bounds are [[x, z], [x, z]] corners.
        pts = [self.point(bounds[i][0], bounds[j][1]) for i in (0, 1) for j in (0, 1)]
        xs, ys = [p[0] for p in pts], [p[1] for p in pts]
        return min(xs), min(ys), max(xs), max(ys)


def render_svg(meta):
    import cairosvg

    svg = fetch_bytes(meta["svgPath"]).decode("utf-8", "replace")
    # Keep only the base layer: drop the groups that upper floors toggle on.
    for layer in meta.get("layers") or []:
        gid = layer.get("svgLayer")
        if gid:
            svg = remove_group(svg, gid)
    m = re.search(r'viewBox="([\d.\s,-]+)"', svg)
    vw, vh = (float(v) for v in re.split(r"[\s,]+", m.group(1).strip())[2:4]) if m else (MAP_PX, MAP_PX)
    scale = MAP_PX / max(vw, vh)
    png = cairosvg.svg2png(bytestring=svg.encode("utf-8"), output_width=round(vw * scale),
                           output_height=round(vh * scale), background_color="#1b1c18")
    return Image.open(io.BytesIO(png)).convert("RGB")


def remove_group(svg, gid):
    start = re.search(r'<g\b[^>]*\bid="%s"[^>]*>' % re.escape(gid), svg)
    if not start:
        return svg
    depth, pos = 1, start.end()
    tag = re.compile(r"<(/?)g\b[^>]*?(/?)>")
    while depth and (m := tag.search(svg, pos)):
        if m.group(1):
            depth -= 1
        elif not m.group(2):
            depth += 1
        pos = m.end()
    return svg[:start.start()] + svg[pos:]


def render_tiles(meta, proj, rect):
    tile = meta.get("tileSize") or 256
    x0, y0, x1, y1 = rect
    # Pick the zoom that makes the long side closest to MAP_PX without going over maxZoom.
    zoom = max(0, min(meta.get("maxZoom") or 6, round(math.log2(MAP_PX / max(x1 - x0, y1 - y0)))))
    s = 2 ** zoom
    px0, py0, px1, py1 = x0 * s, y0 * s, x1 * s, y1 * s
    tx0, ty0 = math.floor(px0 / tile), math.floor(py0 / tile)
    tx1, ty1 = math.floor(px1 / tile), math.floor(py1 / tile)
    canvas = Image.new("RGB", ((tx1 - tx0 + 1) * tile, (ty1 - ty0 + 1) * tile), (27, 28, 24))
    got = 0
    for ty in range(ty0, ty1 + 1):
        for tx in range(tx0, tx1 + 1):
            url = meta["tilePath"].replace("{z}", str(zoom)).replace("{x}", str(tx)).replace("{y}", str(ty))
            try:
                img = Image.open(io.BytesIO(fetch_bytes(url))).convert("RGBA")
            except Exception:  # noqa: BLE001 - tiles outside the map are missing
                continue
            if img.size != (tile, tile):
                img = img.resize((tile, tile))
            canvas.paste(img, ((tx - tx0) * tile, (ty - ty0) * tile), img)
            got += 1
    if not got:
        raise RuntimeError("no tiles")
    ox, oy = tx0 * tile, ty0 * tile
    return canvas.crop((round(px0 - ox), round(py0 - oy), round(px1 - ox), round(py1 - oy)))


def interactive_maps():
    groups = json.loads(fetch_bytes(MAPS_META))
    out = {}
    for g in groups:
        for m in g.get("maps") or []:
            if m.get("projection") == "interactive" and m.get("key"):
                out.setdefault(m["key"], m)
    return out


def boss_name(mob, mobs):
    if isinstance(mobs, dict):
        rec = mobs.get(mob)
        if isinstance(rec, dict) and rec.get("name"):
            return rec["name"]
    for rec in b.records(mobs):
        if mob in (rec.get("id"), rec.get("normalizedName")):
            return rec.get("name") or mob
    return mob


def markers(gm, proj, rect, mobs):
    x0, y0, x1, y1 = rect
    w, h = x1 - x0, y1 - y0

    def uv(pos):
        if not isinstance(pos, dict) or "x" not in pos or "z" not in pos:
            return None
        px, py = proj.point(pos["x"], pos["z"])
        u, v = (px - x0) / w, (py - y0) / h
        if -0.02 <= u <= 1.02 and -0.02 <= v <= 1.02:
            return round(min(max(u, 0), 1), 4), round(min(max(v, 0), 1), 4)
        return None

    out = []
    for e in gm.get("extracts") or []:
        p = uv(e.get("position"))
        if p:
            out.append({"t": {"pmc": "pmc", "scav": "scav"}.get(e.get("faction"), "shared"),
                        "n": e.get("name") or "Extract", "u": p[0], "v": p[1]})
    for t in gm.get("transits") or []:
        p = uv(t.get("position"))
        if p:
            out.append({"t": "transit", "n": t.get("description") or "Transit", "u": p[0], "v": p[1]})
    # Several bosses share spawn zones; one marker per spot lists them all.
    spots = {}
    for boss in gm.get("bosses") or []:
        name = boss_name(boss.get("mob"), mobs)
        if name in PMC_BOTS:
            continue
        for loc in boss.get("spawnLocations") or []:
            pts = [uv(p) for p in loc.get("positions") or []]
            pts = [p for p in pts if p]
            if not pts:
                continue
            u = sum(p[0] for p in pts) / len(pts)
            v = sum(p[1] for p in pts) / len(pts)
            chance = round(100 * (boss.get("spawnChance") or 0) * (loc.get("chance") or 1))
            label = f"{name} {chance}%" if chance else name
            spot = spots.setdefault((round(u, 2), round(v, 2)), {"u": u, "v": v, "names": []})
            if label not in spot["names"]:
                spot["names"].append(label)
    for spot in spots.values():
        out.append({"t": "boss", "n": ", ".join(spot["names"]), "u": round(spot["u"], 4), "v": round(spot["v"], 4)})
    return out


class MapProjector:
    """Converts game positions on any map to image coordinates of that map's picture."""

    def __init__(self, game_maps, meta_by_key):
        self.by_id = {}
        for gm in game_maps:
            key = gm.get("normalizedName")
            meta = meta_by_key.get(MAP_ALIASES.get(key, key))
            if key in SKIP_MAPS or not meta:
                continue
            proj = Projection(meta)
            self.by_id[gm.get("id")] = (key, proj, proj.rect(meta.get("svgBounds") or meta["bounds"]))

    def uv(self, map_id, pos):
        entry = self.by_id.get(map_id)
        if not entry or not isinstance(pos, dict) or "x" not in pos or "z" not in pos:
            return None
        key, proj, (x0, y0, x1, y1) = entry
        px, py = proj.point(pos["x"], pos["z"])
        u, v = (px - x0) / (x1 - x0), (py - y0) / (y1 - y0)
        if -0.02 <= u <= 1.02 and -0.02 <= v <= 1.02:
            return key, round(min(max(u, 0), 1), 4), round(min(max(v, 0), 1), 4)
        return None


def load_maps(mode):
    data = b.fetch_translated(mode, "maps") or {}
    game_maps = b.records(data.get("maps") if isinstance(data, dict) else data)
    return data, game_maps, interactive_maps()


def build_maps(mode, out, loaded=None):
    name = "pvp" if mode == "regular" else mode
    maps_dir = os.path.join(out, "maps")
    os.makedirs(maps_dir, exist_ok=True)
    data, game_maps, meta_by_key = loaded or load_maps(mode)
    mobs = data.get("mobs") if isinstance(data, dict) else None

    entries = []
    for gm in game_maps:
        key = gm.get("normalizedName")
        if key in SKIP_MAPS:
            continue
        img_key = MAP_ALIASES.get(key, key)
        meta = meta_by_key.get(img_key)
        if not meta:
            print(f"  [{name}] no interactive map for {key}", file=sys.stderr)
            continue
        proj = Projection(meta)
        rect = proj.rect(meta.get("svgBounds") or meta["bounds"])
        img_file = f"maps/{img_key}.jpg"
        path = os.path.join(out, img_file)
        if not os.path.exists(path):
            try:
                img = render_svg(meta) if meta.get("svgPath") else render_tiles(meta, proj, rect)
            except Exception as e:  # noqa: BLE001
                print(f"  [{name}] {img_key} render failed: {e}", file=sys.stderr)
                continue
            # Leaflet stretches the image over the bounds, so match their aspect ratio.
            aspect = (rect[2] - rect[0]) / (rect[3] - rect[1])
            size = (MAP_PX, round(MAP_PX / aspect)) if aspect >= 1 else (round(MAP_PX * aspect), MAP_PX)
            img = img.resize(size, Image.LANCZOS)
            img.save(path, quality=85, optimize=True)
        with Image.open(path) as im:
            w, h = im.size
        mk = markers(gm, proj, rect, mobs)
        entries.append({"id": key, "name": gm.get("name") or key, "img": img_file, "w": w, "h": h,
                        "dur": gm.get("raidDuration") or 0, "players": gm.get("players") or "",
                        "markers": mk})
        print(f"  [{name}] {key}: {img_file} {w}x{h}, {len(mk)} markers")
    order = ["ground-zero", "ground-zero-21", "factory", "night-factory", "customs", "woods", "shoreline",
             "interchange", "reserve", "lighthouse", "streets-of-tarkov", "the-lab", "the-lab-dark",
             "the-labyrinth", "terminal", "icebreaker"]
    entries.sort(key=lambda e: (order.index(e["id"]) if e["id"] in order else 99, e["name"]))
    path = os.path.join(out, f"maps-{name}.json")
    with open(path, "w", encoding="utf-8") as f:
        json.dump({"version": 1, "maps": entries}, f, ensure_ascii=False, separators=(",", ":"))
    print(f"wrote {path}: {len(entries)} maps")
