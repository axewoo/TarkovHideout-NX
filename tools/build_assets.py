"""Item icon atlas and map images for the Switch app (used by build_data.py --assets).

Icons: every item the hideout data references is packed into one PNG atlas of
64 px cells, so the app makes a single download and a single texture.

Maps: tarkov.dev's interactive map metadata (the-hideout/tarkov-dev maps.json)
says how game coordinates project onto each map image. Each map is rendered
once as an overview plus chunked detail levels (see MAP_FORMAT), and extracts, transits and
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
from concurrent.futures import ThreadPoolExecutor

from PIL import Image, ImageChops, ImageFilter, ImageOps

import build_data as b

ICON_PX = 64
ATLAS_COLS = 32
PAGE_ICONS = ATLAS_COLS * ATLAS_COLS
CELL_RGB = (36, 37, 32)  # col::cell in the app
# Maps are published as a small overview image plus levels of CHUNK-sized JPEG pieces, so the
# app can show source detail when zoomed in without holding a whole huge image in memory.
MAP_FORMAT = 2
CHUNK = 1024
OVERVIEW_PX = 2048
HI_MAX_PX = 11000        # long side of the sharpest level for full maps
SPARSE_MAPS = {"icebreaker"}  # mostly sea: the full source resolution costs little
SVG_HI_PX = 8192
BG_RGB = (27, 28, 24)
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
            if e.code == 429:
                # Rate limited (the wiki): back off longer than for network errors.
                time.sleep(int(e.headers.get("Retry-After") or 0) or 15 * (attempt + 1))
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


_svg_cache = {}


def render_svg(meta, keep=None, px=SVG_HI_PX):
    """Renders the base plan (long side px), plus the floor group `keep` when given."""
    import cairosvg

    url = meta["svgPath"]
    if url not in _svg_cache:
        _svg_cache[url] = fetch_bytes(url).decode("utf-8", "replace")
    svg = _svg_cache[url]
    # Drop the groups that floors toggle on (all of them for the base plan).
    for layer in meta.get("layers") or []:
        gid = layer.get("svgLayer")
        if gid and gid != keep:
            svg = remove_group(svg, gid)
    m = re.search(r'viewBox="([\d.\s,-]+)"', svg)
    vw, vh = (float(v) for v in re.split(r"[\s,]+", m.group(1).strip())[2:4]) if m else (px, px)
    scale = px / max(vw, vh)
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


def render_tiles(meta, rect, zoom, tile_path=None, box=None):
    """Stitches a tile layer over rect at zoom (rect units * 2**zoom pixels).

    box (pixels of that full image) limits the result to a sub-rectangle.
    With tile_path (a floor) the result keeps its transparency.
    """
    tile = meta.get("tileSize") or 256
    s = 2 ** zoom
    fx0, fy0 = rect[0] * s, rect[1] * s
    full = (round((rect[2] - rect[0]) * s), round((rect[3] - rect[1]) * s))
    bx0, by0, bx1, by1 = box or (0, 0, full[0], full[1])
    px0, py0, px1, py1 = fx0 + bx0, fy0 + by0, fx0 + bx1, fy0 + by1
    tx0, ty0 = math.floor(px0 / tile), math.floor(py0 / tile)
    tx1, ty1 = math.floor((px1 - 1) / tile), math.floor((py1 - 1) / tile)
    canvas_size = ((tx1 - tx0 + 1) * tile, (ty1 - ty0 + 1) * tile)
    canvas = Image.new("RGBA", canvas_size, (0, 0, 0, 0)) if tile_path else Image.new("RGB", canvas_size, BG_RGB)

    def fetch_tile(t):
        url = (tile_path or meta["tilePath"]).replace("{z}", str(zoom)).replace("{x}", str(t[0])).replace("{y}", str(t[1]))
        try:
            return t, Image.open(io.BytesIO(fetch_bytes(url))).convert("RGBA")
        except Exception:  # noqa: BLE001 - tiles outside the map are missing
            return t, None

    got = 0
    coords = [(tx, ty) for ty in range(ty0, ty1 + 1) for tx in range(tx0, tx1 + 1)]
    with ThreadPoolExecutor(8) as pool:
        for (tx, ty), img in pool.map(fetch_tile, coords):
            if img is None:
                continue
            if img.size != (tile, tile):
                img = img.resize((tile, tile))
            if tile_path:
                canvas.alpha_composite(img, ((tx - tx0) * tile, (ty - ty0) * tile))
            else:
                canvas.paste(img, ((tx - tx0) * tile, (ty - ty0) * tile), img)
            got += 1
    if not got and not tile_path:
        raise RuntimeError("no tiles")
    ox, oy = tx0 * tile, ty0 * tile
    return canvas.crop((round(px0 - ox), round(py0 - oy), round(px0 - ox) + (bx1 - bx0), round(py0 - oy) + (by1 - by0)))


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

    def zone(e):
        # The extract zone is where you stand; position can be elsewhere (Icebreaker's helicopter).
        pts = [q for q in e.get("outline") or [] if isinstance(q, dict) and "x" in q and "z" in q]
        if pts:
            return {"x": sum(q["x"] for q in pts) / len(pts), "z": sum(q["z"] for q in pts) / len(pts)}
        return e.get("position")

    out = []
    for e in gm.get("extracts") or []:
        p = uv(zone(e))
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


FLOOR_DIM = 0.45  # base brightness under a floor; the app dims with black at alpha 255 * (1 - FLOOR_DIM)
BELOW_GROUND = ("underground", "tunnel", "bunker", "garage", "basement", "technical")
THUMB_SIZE = (296, 112)
_map_cache = {}


def _height(layer):
    # "2nd Floor" style names order floors; extents only matter for unnumbered ones (Icebreaker).
    m = re.search(r"(\d+)(?:st|nd|rd|th)", layer.get("name", ""))
    if m:
        return 1000 * int(m.group(1))
    ext = layer.get("extents") or [{}]
    return (ext[0].get("height") or [0])[0]


def fit_size(rect, long_side):
    """Pixel size with the rect's aspect (Leaflet stretches images over the bounds)."""
    aspect = (rect[2] - rect[0]) / (rect[3] - rect[1])
    return (long_side, round(long_side / aspect)) if aspect >= 1 else (round(long_side * aspect), long_side)


def level_sizes(size):
    """Chunked level sizes, coarsest first, halving from the sharpest down to about 4k."""
    sizes = [size]
    while max(sizes[-1]) > 4500:
        sizes.append((round(sizes[-1][0] / 2), round(sizes[-1][1] / 2)))
    return sizes[::-1]


def write_chunks(img, folder, level_size, origin=(0, 0), skip_empty=True):
    """Saves the CHUNK cells img covers; img's top-left sits at origin (a chunk corner) in level pixels.

    Returns the set of written cell indices (cy * cols + cx).
    """
    cols = math.ceil(level_size[0] / CHUNK)
    ox, oy = origin
    have = set()
    for y in range(0, img.height, CHUNK):
        for x in range(0, img.width, CHUNK):
            piece = img.crop((x, y, min(img.width, x + CHUNK), min(img.height, y + CHUNK)))
            if skip_empty:
                diff = ImageChops.difference(piece, Image.new("RGB", piece.size, BG_RGB)).convert("L")
                if diff.getextrema()[1] <= 6:
                    continue
            cx, cy = (ox + x) // CHUNK, (oy + y) // CHUNK
            os.makedirs(folder, exist_ok=True)
            piece.save(os.path.join(folder, f"{cx}_{cy}.jpg"), quality=82)
            have.add(cy * cols + cx)
    return have


def have_hex(have):
    return format(sum(1 << i for i in have), "x") if have else "0"


def publish_levels(hi, folder, out, overview_path):
    """Writes the overview and chunk levels of a full-frame image; returns (overview size, levels)."""
    levels = []
    for i, size in enumerate(level_sizes(hi.size)):
        img = hi if size == hi.size else hi.resize(size, Image.LANCZOS)
        have = write_chunks(img, os.path.join(out, folder, str(i)), size)
        levels.append([size[0], size[1], have_hex(have)])
    ov = hi.resize(fit_size((0, 0, hi.width, hi.height), OVERVIEW_PX), Image.LANCZOS)
    ov.save(os.path.join(out, overview_path), quality=85, optimize=True)
    return ov.size, levels


_base_svg = {}


def base_svg(meta, px, size):
    """The plan without floors, cached per map while its floors are built."""
    key = (meta["svgPath"], px, size)
    if key not in _base_svg:
        _base_svg[key] = render_svg(meta, px=px).resize(size, Image.LANCZOS)
    return _base_svg[key]


def svg_mask(with_layer, base):
    diff = ImageChops.difference(with_layer, base).convert("L")
    return diff.point(lambda v: 255 if v > 12 else 0).filter(ImageFilter.MaxFilter(5))


def floor_overlay_box(meta, rect, layer, style, z_hi, hi_size):
    """Where a floor has content, as a 0..1 box, found on a cheap low-resolution render."""
    if layer.get("tilePath") and (style == "render" or not layer.get("svgLayer")):
        z = max(0, z_hi - 3)
        low = render_tiles(meta, rect, z, layer["tilePath"])
        box = low.getchannel("A").getbbox()
        size = low.size
    elif layer.get("svgLayer"):
        size = fit_size(rect, OVERVIEW_PX)
        base = base_svg(meta, OVERVIEW_PX, size)
        box = svg_mask(render_svg(meta, keep=layer["svgLayer"], px=OVERVIEW_PX).resize(size, Image.LANCZOS),
                       base).getbbox()
    else:
        return None
    if not box:
        return None
    pad = 2
    return (max(0, (box[0] - pad) / size[0]), max(0, (box[1] - pad) / size[1]),
            min(1, (box[2] + pad) / size[0]), min(1, (box[3] + pad) / size[1]))


def floor_overlay(meta, rect, layer, style, z_hi, hi, box_px):
    """The floor's own pixels inside box_px (hi-level pixels), as RGBA."""
    if style == "render" and layer.get("tilePath"):
        return render_tiles(meta, rect, z_hi, layer["tilePath"], box_px)
    if style == "render" and layer.get("svgLayer") and meta.get("svgPath"):
        # A floor only drawn as a plan, shown over the render.
        style = "plan"
    if layer.get("svgLayer") and meta.get("svgPath"):
        size = fit_size(rect, SVG_HI_PX)
        base = base_svg(meta, SVG_HI_PX, size)
        with_layer = render_svg(meta, keep=layer["svgLayer"]).resize(size, Image.LANCZOS)
        if size != hi.size:
            base, with_layer = base.resize(hi.size, Image.LANCZOS), with_layer.resize(hi.size, Image.LANCZOS)
        base, with_layer = base.crop(box_px), with_layer.crop(box_px)
        with_layer.putalpha(svg_mask(with_layer, base))
        return with_layer
    if layer.get("tilePath") and meta.get("tilePath"):
        # A floor only drawn as a render, shown over the plan: render at the plan's scale.
        z = round(math.log2(hi.width / (rect[2] - rect[0])))
        scale = (2 ** z) * (rect[2] - rect[0]) / hi.width
        b = tuple(round(v * scale) for v in box_px)
        return render_tiles(meta, rect, z, layer["tilePath"], b).resize(
            (box_px[2] - box_px[0], box_px[3] - box_px[1]), Image.LANCZOS)
    return None


def publish_floor(hi, overlay, box_px, folder, out, overview_path):
    """Writes a floor: the dimmed base with the floor on top, as an overview crop plus chunk levels
    covering only the floor. Returns (r, levels) or None when the floor is empty."""
    alpha = overlay.getchannel("A")
    inner = alpha.getbbox()
    if not inner:
        return None
    box_px = (box_px[0] + inner[0], box_px[1] + inner[1], box_px[0] + inner[2], box_px[1] + inner[3])
    overlay = overlay.crop(inner)
    W, H = hi.size
    r = [round(box_px[0] / W, 5), round(box_px[1] / H, 5), round(box_px[2] / W, 5), round(box_px[3] / H, 5)]
    levels = []
    for i, size in enumerate(level_sizes(hi.size) + [fit_size((0, 0, W, H), OVERVIEW_PX)]):
        sx, sy = size[0] / W, size[1] / H
        b = (math.floor(box_px[0] * sx), math.floor(box_px[1] * sy), math.ceil(box_px[2] * sx), math.ceil(box_px[3] * sy))
        is_overview = i == len(level_sizes(hi.size))
        # Chunk levels start on a cell corner so the pieces line up with the base map's.
        region = b if is_overview else ((b[0] // CHUNK) * CHUNK, (b[1] // CHUNK) * CHUNK,
                                        min(size[0], -(-b[2] // CHUNK) * CHUNK), min(size[1], -(-b[3] // CHUNK) * CHUNK))
        src = (region[0] / sx, region[1] / sy, region[2] / sx, region[3] / sy)
        base = hi.crop(tuple(round(v) for v in src)).resize((region[2] - region[0], region[3] - region[1]), Image.LANCZOS)
        comp = Image.eval(base, lambda v: int(v * FLOOR_DIM))
        ov = overlay.resize((max(1, b[2] - b[0]), max(1, b[3] - b[1])), Image.LANCZOS)
        comp.paste(ov.convert("RGB"), (b[0] - region[0], b[1] - region[1]), ov.getchannel("A"))
        if is_overview:
            comp.save(os.path.join(out, overview_path), quality=82, optimize=True)
        else:
            have = write_chunks(comp, os.path.join(out, folder, str(i)), size, region[:2], skip_empty=False)
            levels.append([size[0], size[1], have_hex(have)])
    return r, levels


def build_map_images(meta, proj, rect, img_key, out):
    """Renders a map's styles and floors (cached per image key, and on disk across runs).

    Returns {"styles": [{"name", "img", "w", "h", "lv"}], "floors": [{"n", "s": [{"img", "r", "lv"} | None]}]},
    floors bottom to top with an entry (no "s") for the base level.
    """
    if img_key in _map_cache:
        return _map_cache[img_key]
    meta_path = os.path.join(out, "maps", img_key, "meta.json")
    if os.path.exists(meta_path):
        with open(meta_path, encoding="utf-8") as f:
            cached = json.load(f)
        if cached.get("format") == MAP_FORMAT and cached.get("source") == meta.get("tilePath", "") + meta.get("svgPath", ""):
            _map_cache[img_key] = cached
            return cached
    os.makedirs(os.path.join(out, "maps", img_key), exist_ok=True)
    sparse = img_key in SPARSE_MAPS
    ext = max(rect[2] - rect[0], rect[3] - rect[1])
    z_hi = meta.get("maxZoom") or 6
    while not sparse and z_hi > 0 and ext * 2 ** z_hi > HI_MAX_PX:
        z_hi -= 1

    layers = [l for l in meta.get("layers") or [] if l.get("tilePath") or l.get("svgLayer")]
    base_layer = next((l for l in layers if l.get("tilePath") and l["tilePath"] == meta.get("tilePath")), None)
    layers = [l for l in layers if l is not base_layer]

    styles, floor_sets = [], {id(l): [] for l in layers}
    # The game render (tiles) is the main style where it exists; the vector plan is the alternative.
    for style in [n for n, k in (("render", "tilePath"), ("plan", "svgPath")) if meta.get(k)]:
        try:
            hi = render_tiles(meta, rect, z_hi) if style == "render" else \
                base_svg(meta, SVG_HI_PX, fit_size(rect, SVG_HI_PX))
        except Exception as e:  # noqa: BLE001
            print(f"  {img_key} {style} render failed: {e}", file=sys.stderr)
            continue
        folder = f"maps/{img_key}/{style}"
        (w, h), levels = publish_levels(hi, folder, out, folder + ".jpg")
        styles.append({"name": style, "img": folder + ".jpg", "w": w, "h": h, "lv": levels})
        for layer in layers:
            slug = re.sub(r"[^a-z0-9]+", "-", layer["name"].lower()).strip("-")
            entry = None
            try:
                box = floor_overlay_box(meta, rect, layer, style, z_hi, hi.size)
                if not box and style == "render":
                    box = floor_overlay_box(meta, rect, layer, "plan", z_hi, hi.size)
                if box:
                    box_px = (math.floor(box[0] * hi.width), math.floor(box[1] * hi.height),
                              math.ceil(box[2] * hi.width), math.ceil(box[3] * hi.height))
                    overlay = floor_overlay(meta, rect, layer, style, z_hi, hi, box_px)
                    if overlay is not None:
                        ffolder = f"{folder}-f-{slug}"
                        res = publish_floor(hi, overlay, box_px, ffolder, out, ffolder + ".jpg")
                        if res:
                            entry = {"img": ffolder + ".jpg", "r": res[0], "lv": res[1]}
            except Exception as e:  # noqa: BLE001
                print(f"  {img_key} floor {layer['name']} ({style}) failed: {e}", file=sys.stderr)
            floor_sets[id(layer)].append(entry)
        del hi
    _base_svg.clear()

    floors = [{"n": l["name"], "h": _height(l), "s": floor_sets[id(l)]}
              for l in layers if any(floor_sets[id(l)])]
    if floors:
        if base_layer:
            base_h = _height(base_layer)
            below = [f for f in floors if f["h"] < base_h]
            above = [f for f in floors if f["h"] >= base_h]
        else:
            below = [f for f in floors if any(k in f["n"].lower() for k in BELOW_GROUND)]
            above = [f for f in floors if f not in below]
        below.sort(key=lambda f: f["h"])
        above.sort(key=lambda f: f["h"])
        floors = below + [{"n": base_layer["name"] if base_layer else "Ground level"}] + above
        for f in floors:
            f.pop("h", None)
    result = {"format": MAP_FORMAT, "source": meta.get("tilePath", "") + meta.get("svgPath", ""),
              "styles": styles, "floors": floors}
    if styles:
        with Image.open(os.path.join(out, styles[0]["img"])) as im:
            ImageOps.fit(im.convert("RGB"), THUMB_SIZE, Image.LANCZOS).save(
                os.path.join(out, "maps", img_key, "thumb.jpg"), quality=85)
        with open(meta_path, "w", encoding="utf-8") as f:
            json.dump(result, f, separators=(",", ":"))
    _map_cache[img_key] = result
    return result


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
        imgs = build_map_images(meta, proj, rect, img_key, out)
        styles = imgs["styles"]
        if not styles:
            continue
        # img/alt/w/h and floors' img/r/alt/ar are what app versions before tiled levels read.
        floors = []
        for fl in imgs["floors"]:
            f = {"n": fl["n"]}
            for i, sv in enumerate(fl.get("s") or []):
                if sv:
                    pre = "a" if i else ""
                    f["alt" if i else "img"] = sv["img"]
                    f[pre + "r"] = sv["r"]
                    f[pre + "lv"] = sv["lv"]
            if "s" not in fl or "img" in f or "alt" in f:
                if "s" in fl and "img" not in f:  # only drawn in the second style
                    f["img"], f["r"], f["lv"] = f.pop("alt"), f.pop("ar"), f.pop("alv")
                floors.append(f)
        mk = markers(gm, proj, rect, mobs)
        # Icebreaker's helicopter extract is given where the helicopter hovers, off the ship;
        # you extract from the helipad, the octagon at the far (bow) end of the Helipad level.
        pad = next((f for f in floors if "helipad" in f["n"].lower() and "r" in f), None)
        if pad:
            r = pad["r"]
            for m in mk:
                if "heli" in m["n"].lower() and m["t"] in ("pmc", "scav", "shared"):
                    top = m["v"] <= (r[1] + r[3]) / 2
                    m["u"] = round((r[0] + r[2]) / 2, 4)
                    m["v"] = round(r[1] + 0.07 * (r[3] - r[1]) if top else r[3] - 0.07 * (r[3] - r[1]), 4)
        entries.append({"id": key, "name": gm.get("name") or key, "img": styles[0]["img"], "lv": styles[0]["lv"],
                        "alt": styles[1]["img"] if len(styles) > 1 else "",
                        "alv": styles[1]["lv"] if len(styles) > 1 else [],
                        "thumb": f"maps/{img_key}/thumb.jpg", "floors": floors,
                        "w": styles[0]["w"], "h": styles[0]["h"],
                        "dur": gm.get("raidDuration") or 0, "players": gm.get("players") or "",
                        "markers": mk})
        print(f"  [{name}] {key}: {[(st['name'], st['lv'][-1][:2]) for st in styles]}, {len(mk)} markers, "
              f"floors {[f['n'] for f in floors]}")
    order = ["ground-zero", "ground-zero-21", "factory", "night-factory", "customs", "woods", "shoreline",
             "interchange", "reserve", "lighthouse", "streets-of-tarkov", "the-lab", "the-lab-dark",
             "the-labyrinth", "terminal", "icebreaker"]
    entries.sort(key=lambda e: (order.index(e["id"]) if e["id"] in order else 99, e["name"]))
    path = os.path.join(out, f"maps-{name}.json")
    with open(path, "w", encoding="utf-8") as f:
        json.dump({"version": 2, "maps": entries}, f, ensure_ascii=False, separators=(",", ":"))
    print(f"wrote {path}: {len(entries)} maps")
