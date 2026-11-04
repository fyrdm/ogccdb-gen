#!/usr/bin/env python3
"""Build a sample OGC CDB 1.3 data store from open elevation and imagery data.

Reads a small area of interest straight from the source rasters (Cloud Optimized
GeoTIFFs over HTTPS, through GDAL's /vsicurl/), so only that window is downloaded,
then runs ogccdb-gen. Sources, area and attribution come from a JSON config; see
sources.template.json and README.md next to this script.

Needs the GDAL command line tools (gdalbuildvrt, gdal_translate, gdalwarp,
gdalinfo, gdallocationinfo, gdaltransform) and PROJ's cs2cs. Python standard library only.
"""

import argparse
import json
import math
import re
import shutil
import subprocess
import sys
from collections import Counter
from pathlib import Path

DEM_MARGIN_PX = 2  # source posts kept around the area, so its edges are covered
IMAGE_MARGIN_DEG = 0.002
MAX_IGNORED_NODATA_PERCENT = 0.05
# Areas outside the DEM sources: without a nodata tag they would read as 0 m and be
# imported as real heights. Tagged as nodata, the tiles they touch are skipped.
MISSING_HEIGHT = -32767


def run(cmd, capture=False, stdin=None):
    print("+ " + " ".join(str(c) if len(str(c)) < 80 else "'...'" for c in cmd), flush=True)
    res = subprocess.run([str(c) for c in cmd], check=True, text=True, input=stdin,
                         stdout=subprocess.PIPE if capture else None)
    return res.stdout if capture else None


def fail(msg):
    sys.exit("build_sample_cdb: " + msg)


def load_config(path):
    cfg = json.loads(Path(path).read_text(encoding="utf-8"))
    a = cfg["area"]
    if not (-180 <= a["west"] < a["east"] <= 180 and -90 <= a["south"] < a["north"] <= 90):
        fail("area must have west < east and south < north, in degrees")
    for key in ("elevation", "imagery"):
        urls = [u for u in cfg[key].get("urls", []) if u]
        if not urls:
            fail(f"{path}: '{key}.urls' is empty; fill in the source URLs (see README.md)")
        cfg[key]["urls"] = urls
    return cfg


def source_vrt(urls, out, fill=None):
    """One virtual raster over the remote sources (they must share a CRS).

    With fill, areas no source covers read as that value, tagged as nodata.
    """
    if not out.exists():
        opts = [] if fill is None else ["-vrtnodata", fill]
        run(["gdalbuildvrt", "-q"] + opts + [out] + ["/vsicurl/" + u for u in urls])
    return out


def raster_info(path, stats=False):
    return json.loads(run(["gdalinfo", "-json"] + (["-stats"] if stats else []) + [path],
                          capture=True))


def fetch_elevation(cfg, area, work):
    """Copy the source posts that cover the area, without resampling."""
    out = work / "elevation_source.tif"
    if out.exists():
        return out
    vrt = source_vrt(cfg["urls"], work / "elevation_source.vrt", fill=MISSING_HEIGHT)
    gt = raster_info(vrt)["geoTransform"]
    mx, my = DEM_MARGIN_PX * abs(gt[1]), DEM_MARGIN_PX * abs(gt[5])
    run(["gdal_translate", "-q", "-projwin",
         area["west"] - mx, area["north"] + my, area["east"] + mx, area["south"] - my,
         "-co", "COMPRESS=DEFLATE", "-co", "TILED=YES", vrt, out])
    return out


def height_at(path, lon, lat):
    v = run(["gdallocationinfo", "-valonly", "-wgs84", path, lon, lat], capture=True)
    return float(v.split()[0])


def geoid_shift(vertical_crs, lon, lat):
    """Ellipsoid height of a point at 0 m on the geoid, or None if PROJ lacks the grid."""
    res = subprocess.run(["cs2cs", "-d", "3", "--only-best", f"EPSG:4326+{vertical_crs}",
                          "EPSG:4979"], input=f"{lat} {lon} 0\n", text=True,
                         capture_output=True)
    try:
        h = float(res.stdout.split()[2])
    except (IndexError, ValueError):
        return None
    return h if math.isfinite(h) else None


def to_ellipsoid(src, vertical_crs, area, work):
    """Convert geoid heights to WGS-84 ellipsoid heights, on the same grid.

    CDB elevation is relative to the WGS-84 reference ellipsoid (CDB 1.3 Vol 1
    §10.6.1); ogccdb-core takes source heights as given.
    """
    out = work / "elevation_ellipsoid.tif"
    if out.exists():
        return out
    lon = (area["west"] + area["east"]) / 2
    lat = (area["south"] + area["north"]) / 2
    shift = geoid_shift(vertical_crs, lon, lat)
    if shift is None:
        fail(f"PROJ cannot convert {vertical_crs} heights here: its geoid grid is missing. "
             "Install the grid (projsync, or PROJ_NETWORK=ON) or pass --keep-geoid-heights.")
    info = raster_info(src)
    gt, (w, h) = info["geoTransform"], info["size"]
    run(["gdalwarp", "-q", "-s_srs", f"EPSG:4326+{vertical_crs}", "-t_srs", "EPSG:4979",
         "-te", gt[0], gt[3] + h * gt[5], gt[0] + w * gt[1], gt[3], "-ts", w, h,
         "-r", "near", "-co", "COMPRESS=DEFLATE", "-co", "TILED=YES", src, out])
    # gdalwarp silently skips the vertical step when it cannot do it: check one post.
    got = height_at(out, lon, lat) - height_at(src, lon, lat)
    if abs(got - shift) > 0.5:
        out.unlink()
        fail(f"geoid conversion check failed: expected {shift:+.2f} m, got {got:+.2f} m")
    print(f"geoid to ellipsoid: {shift:+.2f} m at the area centre")
    return out


def projected_bounds(area, info):
    """Bounds, in the dataset's CRS, of the area plus a margin.

    Transforms points along all four edges: gdalwarp -te_srs only transforms two
    corners, which cuts off corners of the area when the grid is rotated (UTM).
    """
    m, n = IMAGE_MARGIN_DEG, 8
    w, s, e, nn = area["west"] - m, area["south"] - m, area["east"] + m, area["north"] + m
    pts = []
    for i in range(n + 1):
        x, y = w + (e - w) * i / n, s + (nn - s) * i / n
        pts += [(x, s), (x, nn), (w, y), (e, y)]
    out = run(["gdaltransform", "-s_srs", "EPSG:4326", "-t_srs", info["coordinateSystem"]["wkt"],
               "-output_xy"],
              capture=True, stdin="".join(f"{x} {y}\n" for x, y in pts))
    xy = [tuple(map(float, line.split())) for line in out.splitlines() if line.strip()]
    xs, ys = [p[0] for p in xy], [p[1] for p in xy]
    return [min(xs), min(ys), max(xs), max(ys)]


def fetch_imagery(cfg, area, work):
    """Cut the area from the scene in its own CRS and pixel grid (no resampling)."""
    out = work / "imagery_source.tif"
    if out.exists():
        return out
    vrt = source_vrt(cfg["urls"], work / "imagery_source.vrt")
    info = raster_info(vrt)
    gt = info["geoTransform"]
    run(["gdalwarp", "-q", "-te"] + projected_bounds(area, info) +
        ["-tr", abs(gt[1]), abs(gt[5]), "-tap", "-r", "near",
         "-co", "COMPRESS=DEFLATE", "-co", "TILED=YES", "-co", "PHOTOMETRIC=RGB", vrt, out])
    return out


def ignore_nodata(src, area, work):
    """Treat the source's nodata value as data.

    Some 8-bit scenes use 0, 0, 0 both for "no data" and for real black pixels,
    such as deep cloud shadow; a cluster of them makes ogccdb-gen skip the tile.
    Allowed only when the area lies inside the scenes: its corners and edge
    midpoints hold data and nearly the whole window is valid, so the nodata
    pixels are dark pixels, not missing data.
    """
    mask = work / "imagery_source_mask.vrt"
    run(["gdal_translate", "-q", "-of", "VRT", "-b", "mask", src, mask])
    band = raster_info(mask, stats=True)["bands"][0]
    valid = 100 * float(band["metadata"][""]["STATISTICS_MEAN"]) / 255
    w, s, e, n = (area[k] for k in ("west", "south", "east", "north"))
    probes = [(x, y) for x in (w, (w + e) / 2, e) for y in (s, (s + n) / 2, n)]
    edges_ok = all(run(["gdallocationinfo", "-valonly", "-wgs84", mask, x, y],
                       capture=True).strip() == "255" for x, y in probes)
    if not edges_ok or valid < 100 - MAX_IGNORED_NODATA_PERCENT:
        fail(f"{src}: the area leaves the scenes ({valid:.3f}% of the window is valid data), "
             "so its nodata cannot be ignored. Add a scene that covers the area.")
    out = work / "imagery_source_all_valid.vrt"
    run(["gdal_translate", "-q", "-of", "VRT", "-a_nodata", "none", src, out])
    print(f"imagery: nodata ignored ({100 - valid:.4f}% of the window held the nodata value)")
    return out


def lod_of(name):
    m = re.search(r"_L(C?)(\d+)_", name)
    return -int(m.group(2)) if m.group(1) else int(m.group(2))


def geocell_width(lat):
    """Geocell width in degrees of longitude for the slice at `lat` (CDB 1.3 Vol 1 §8.6.2.1)."""
    a = lat if lat >= 0 else -lat - 1  # distance of the slice from the equator
    for limit, width in ((50, 1), (70, 2), (75, 3), (80, 4), (89, 6)):
        if a < limit:
            return width
    return 12


def area_share(area, geocell):
    """Share of the geocell ("N47/E008") that the area covers, 0..1."""
    lat = int(geocell[1:3]) * (1 if geocell[0] == "N" else -1)
    lon = int(geocell[5:8]) * (1 if geocell[4] == "E" else -1)
    w = geocell_width(lat)
    dx = min(area["east"], lon + w) - max(area["west"], lon)
    dy = min(area["north"], lat + 1) - max(area["south"], lat)
    return max(dx, 0) * max(dy, 0) / w


def summary(gen, cdb, area):
    """Tiles written per dataset, LOD and geocell, out of the geocell's tiles at that LOD."""
    paths = run([gen, "list", cdb], capture=True).split()
    # Tiles/<lat>/<lon>/<dataset>/<LOD>/<U>/<file> (CDB 1.3 Vol 1 §8.6.2)
    counts = Counter((p.split("/")[3], lod_of(Path(p).name), "/".join(p.split("/")[1:3]))
                     for p in paths)
    print(f"\n{len(paths)} tiles in {cdb}, area {area['west']}..{area['east']} E, "
          f"{area['south']}..{area['north']} N")
    partial = False
    for geocell in sorted({g for _, _, g in counts}):
        share = area_share(area, geocell)
        if share < 1:
            partial = True
            print(f"  {geocell}: the area covers only {share:.0%} of this geocell")
    for (dataset, lod, geocell), n in sorted(counts.items()):
        # A geocell has 2^LOD x 2^LOD tiles at LOD >= 0 and one below (Vol 1 §8.6.2.5).
        total = 4 ** max(lod, 0)
        partial = partial or n < total
        note = "" if n == total else "  (partial)"
        print(f"  {dataset:<14} LOD {lod:>3}  {geocell}: {n:>4} of {total:>4} tiles{note}")
    if partial:
        print("Partial: some geocells are not covered whole, by the area or by the sources. "
              "Coarse single-pixel tiles can still be written for them.")


def main():
    here = Path(__file__).resolve().parent
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--config", required=True, help="sources JSON (see sources.template.json)")
    ap.add_argument("--gen", default="build/ogccdb-gen", help="path to the ogccdb-gen program")
    ap.add_argument("--work", default="out/sample",
                    help="folder for the downloaded windows and the CDB (default: out/sample)")
    ap.add_argument("--keep-geoid-heights", action="store_true",
                    help="import heights as given, without converting them to the ellipsoid")
    ap.add_argument("--force", action="store_true", help="replace an existing CDB in --work")
    ap.add_argument("--min-lod", type=int, default=0,
                    help="coarsest LOD, passed to ogccdb-gen (default: 0, no negative LODs)")
    ap.add_argument("--max-lod", type=int, help="passed to ogccdb-gen (default: source resolution)")
    args = ap.parse_args()

    cfg = load_config(args.config)
    for tool in ("gdalbuildvrt", "gdal_translate", "gdalwarp", "gdalinfo", "gdallocationinfo",
                 "gdaltransform", "cs2cs"):
        if not shutil.which(tool):
            fail(f"{tool} not found; install the GDAL and PROJ command line tools")
    gen = Path(args.gen)
    if not gen.exists():
        fail(f"{gen} not found; build the repository first or pass --gen")

    work = Path(args.work)
    cdb = work / "CDB"
    if cdb.exists():
        if not args.force:
            fail(f"{cdb} exists; pass --force to rebuild it")
        shutil.rmtree(cdb)
    work.mkdir(parents=True, exist_ok=True)

    area, elev, img = cfg["area"], cfg["elevation"], cfg["imagery"]
    dem = fetch_elevation(elev, area, work)
    vcrs = elev.get("vertical_crs")
    if vcrs and not args.keep_geoid_heights:
        dem = to_ellipsoid(dem, vcrs, area, work)
    image = fetch_imagery(img, area, work)
    if img.get("ignore_nodata"):
        image = ignore_nodata(image, area, work)

    # Credits travel with the data in the Version.xml comment (CDB 1.3 Vol 1 §10.1.7).
    credits = " ".join(c for c in (elev.get("attribution"), img.get("attribution")) if c)
    comment = (f"Sample CDB. Elevation: {elev['description']}. Imagery: {img['description']}. "
               f"Area: {area['west']},{area['south']} to {area['east']},{area['north']}."
               + (f" {credits}" if credits else ""))
    # Only tiles that overlap the area: edge posts would otherwise also fill tiles
    # of neighbouring geocells.
    extent = ",".join(str(area[k]) for k in ("west", "south", "east", "north"))
    cmd = [gen, "build", cdb, "--elevation", dem, "--imagery", image, "--extent", extent,
           "--comment", comment]
    cmd += ["--min-lod", args.min_lod]
    if args.max_lod is not None:
        cmd += ["--max-lod", args.max_lod]
    run(cmd)
    summary(gen, cdb, area)


if __name__ == "__main__":
    main()
