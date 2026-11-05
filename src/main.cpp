// ogccdb-gen: builds an OGC CDB 1.3 data store from GeoTIFF/DEM and RGB
// GeoTIFF inputs, and lists and checks its tiles. Built on the ogccdb-core library.

#include <stdexcept>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <iostream>
#include <locale>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "ogccdb/cdb13/lod.hpp"
#include "ogccdb/cdb13/store.hpp"

namespace fs = std::filesystem;
using namespace ogccdb::cdb13;

namespace {

constexpr std::string_view kUsage =
    "usage:\n"
    "  ogccdb-gen build <cdb> [--elevation <dem.tif|folder>]... [--imagery <image.tif|folder>]...\n"
    "                   [--min-lod N] [--max-lod N] [--resampling nearest|bilinear|cubic|average]\n"
    "                   [--elevation-min-lod N] [--elevation-max-lod N] [--elevation-resampling R]\n"
    "                   [--imagery-min-lod N] [--imagery-max-lod N] [--imagery-resampling R]\n"
    "                   [--quality 1..100] [--extent W,S,E,N] [--no-fill] [--comment TEXT]\n"
    "  ogccdb-gen list <cdb> [--dataset 1|4] [--lod N]\n"
    "  ogccdb-gen info <cdb> [--dataset 1|4] [--lod N]\n"
    "  ogccdb-gen check <cdb> [--dataset 1|4] [--lod N]\n"
    "\n"
    "build creates the CDB Version <cdb> (Metadata/Version.xml) if it does not exist yet,\n"
    "then imports the Elevation and Imagery sources into it. Repeat --elevation or\n"
    "--imagery, or name a folder (its .tif files, by name), to mosaic several inputs of\n"
    "any size, resolution and CRS; where they overlap, the finer one wins (the later one\n"
    "at equal resolution).\n"
    "LODs: by default from LOD -10 to as fine as each input's resolution allows, set per\n"
    "dataset. --min-lod, --max-lod and --resampling apply to both datasets; the\n"
    "--elevation-* and --imagery-* forms set one dataset and win over them.\n"
    "--extent (degrees) imports only the tiles that overlap it, e.g. one geocell:\n"
    "--extent 8,47,9,48.\n"
    "Tiles are written whole (CDB 1.3 Vol 1 §8.6.1.1): where no input covers part of a\n"
    "tile, that part gets the dataset default value from Metadata/Defaults.xml, else\n"
    "the spec's (0 m for Elevation, gray 0.5 for Imagery). --no-fill leaves such tiles\n"
    "out instead, with the finer tiles under them (§7.1.2.2 Requirement 15).\n"
    "list prints the tile files of <cdb>, one path per line.\n"
    "info prints, per dataset component, LOD and geocell, how many of the geocell's tiles\n"
    "<cdb> holds.\n"
    "check reads every file under <cdb>/Tiles and prints one line per problem: a path\n"
    "that is not a CDB tile path, or a tile that cannot be read (format, size, channels,\n"
    "data type). It exits with 1 if it finds a problem.\n";

struct UsageError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

int to_int(const std::string& flag, const std::string& v) {
    std::size_t end = 0;
    int n = 0;
    try {
        n = std::stoi(v, &end);
    } catch (const std::exception&) {
        end = 0;
    }
    if (end == 0 || end != v.size()) throw UsageError(flag + " needs an integer, got '" + v + "'");
    return n;
}

GeoExtent to_extent(const std::string& v) {
    double w = 0, s = 0, e = 0, n = 0;
    char c1 = 0, c2 = 0, c3 = 0, rest = 0;
    std::istringstream in(v);
    in.imbue(std::locale::classic());
    if (!(in >> w >> c1 >> s >> c2 >> e >> c3 >> n) || c1 != ',' || c2 != ',' || c3 != ',' ||
        in >> rest || !(w < e && s < n))
        throw UsageError("--extent needs W,S,E,N in degrees with W < E and S < N, got '" + v + "'");
    return GeoExtent{s, w, n, e};
}

Resampling to_resampling(const std::string& v) {
    if (v == "nearest") return Resampling::nearest;
    if (v == "bilinear") return Resampling::bilinear;
    if (v == "cubic") return Resampling::cubic;
    if (v == "average") return Resampling::average;
    throw UsageError("unknown resampling '" + v + "'");
}

// The .tif/.tiff files of folder `p`, sorted by name, or `p` itself.
std::vector<fs::path> expand(const fs::path& p) {
    if (!fs::is_directory(p)) return {p};
    std::vector<fs::path> out;
    for (const auto& e : fs::directory_iterator(p)) {
        std::string ext = e.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return char(std::tolower(c)); });
        if (e.is_regular_file() && (ext == ".tif" || ext == ".tiff")) out.push_back(e.path());
    }
    if (out.empty()) throw UsageError("no .tif files in " + p.string());
    std::sort(out.begin(), out.end());
    return out;
}

// " (<coarsest LOD> to <finest LOD>)" of the tiles, or nothing if there are none.
std::string lod_range(const std::vector<TileAddress>& tiles) {
    if (tiles.empty()) return "";
    const auto [lo, hi] = std::minmax_element(
        tiles.begin(), tiles.end(), [](const TileAddress& a, const TileAddress& b) { return a.lod < b.lod; });
    return " (" + lod_name(lo->lod) + " to " + lod_name(hi->lod) + ")";
}

void report(std::string_view what, const RasterImportResult& r, PartialTiles partial) {
    std::cout << what << ": " << r.written.size() << " tiles written" << lod_range(r.written);
    if (partial == PartialTiles::skip)
        std::cout << ", " << r.skipped.size() << " left out (partly covered, or under a tile left out)";
    else if (!r.filled.empty())
        std::cout << ", " << r.filled.size() << " of them partly covered and filled with the default value"
                  << lod_range(r.filled);
    std::cout << "\n";
}

// LOD and resampling options given for both datasets and for one of them.
struct DatasetOptions {
    std::optional<int> min_lod, max_lod;
    std::optional<Resampling> resampling;
};

void apply(const DatasetOptions& o, RasterImportOptions& to) {
    if (o.min_lod) to.min_lod = *o.min_lod;
    if (o.max_lod) to.max_lod = *o.max_lod;
    if (o.resampling) to.resampling = *o.resampling;
}

// Parses --<prefix>min-lod, --<prefix>max-lod and --<prefix>resampling.
bool parse_dataset_option(const std::string& prefix, const std::string& flag, const std::string& v,
                          DatasetOptions& o) {
    if (flag == "--" + prefix + "min-lod") o.min_lod = to_int(flag, v);
    else if (flag == "--" + prefix + "max-lod") o.max_lod = to_int(flag, v);
    else if (flag == "--" + prefix + "resampling") o.resampling = to_resampling(v);
    else return false;
    return true;
}

int build(const fs::path& cdb, const std::vector<std::string>& args) {
    std::vector<fs::path> elevation, imagery;
    std::optional<std::string> comment;
    DatasetOptions both, elevation_only, imagery_only;
    ImageryImportOptions imagery_opt;
    std::optional<GeoExtent> extent;
    PartialTiles partial = PartialTiles::fill_default;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& flag = args[i];
        if (flag == "--no-fill") {
            partial = PartialTiles::skip;
            continue;
        }
        if (i + 1 >= args.size()) throw UsageError(flag + " needs a value");
        const std::string& v = args[++i];
        if (flag == "--elevation") {
            const auto files = expand(v);
            elevation.insert(elevation.end(), files.begin(), files.end());
        } else if (flag == "--imagery") {
            const auto files = expand(v);
            imagery.insert(imagery.end(), files.begin(), files.end());
        }
        else if (parse_dataset_option("", flag, v, both)) {}
        else if (parse_dataset_option("elevation-", flag, v, elevation_only)) {}
        else if (parse_dataset_option("imagery-", flag, v, imagery_only)) {}
        else if (flag == "--quality") imagery_opt.encoding.quality = to_int(flag, v);
        else if (flag == "--extent") extent = to_extent(v);
        else if (flag == "--comment") comment = v;
        else throw UsageError("unknown option " + flag);
    }

    const bool exists = fs::exists(cdb / fs::path(std::string(kVersionFile)));
    if (exists && comment) throw UsageError("--comment only applies to a new CDB");
    VersionMetadata version;
    version.comment = comment;
    Store store = exists ? Store::open(cdb) : Store::create(cdb, version);
    std::cout << (exists ? "opened " : "created ") << store.root().string() << "\n";

    RasterImportOptions elevation_opt;
    elevation_opt.extent = imagery_opt.extent = extent;
    elevation_opt.partial = imagery_opt.partial = partial;
    apply(both, elevation_opt);
    apply(elevation_only, elevation_opt);
    apply(both, imagery_opt);
    apply(imagery_only, imagery_opt);
    if (!elevation.empty()) report("Elevation", store.import_elevation(elevation, elevation_opt), partial);
    if (!imagery.empty()) report("Imagery", store.import_imagery(imagery, imagery_opt), partial);
    return 0;
}

TileQuery parse_query(const std::vector<std::string>& args) {
    TileQuery q;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& flag = args[i];
        if (i + 1 >= args.size()) throw UsageError(flag + " needs a value");
        const std::string& v = args[++i];
        if (flag == "--dataset") q.dataset = to_int(flag, v);
        else if (flag == "--lod") q.lod = to_int(flag, v);
        else throw UsageError("unknown option " + flag);
    }
    return q;
}

int list(const fs::path& cdb, const std::vector<std::string>& args) {
    const TileQuery q = parse_query(args);
    const Store store = Store::open(cdb);
    for (const TileFile& f : store.list_tiles(q)) std::cout << relative_path(f) << "\n";
    return 0;
}

std::string geocell_name(Geocell g) { return latitude_name(g.lat) + longitude_name(g.lon); }

int info(const fs::path& cdb, const std::vector<std::string>& args) {
    const TileQuery q = parse_query(args);
    const Store store = Store::open(cdb);
    const std::vector<TileFile> files = store.list_tiles(q);
    const auto& spec = store.version().specification;
    std::cout << store.root().string() << ": CDB " << (spec ? spec->version : "?") << ", "
              << files.size() << " tiles\n";

    // list_tiles() sorts by dataset, selectors, LOD and geocell, so equal keys are adjacent.
    using Key = std::tuple<int, int, int, int, int, int>;  // dataset, S, T, LOD, lat, lon
    const auto key = [](const TileFile& f) {
        return Key{f.dataset, f.selector1, f.selector2, f.tile.lod, f.tile.geocell.lat,
                   f.tile.geocell.lon};
    };
    std::optional<std::tuple<int, int, int>> component;
    bool partial = false;
    for (std::size_t i = 0; i < files.size();) {
        std::size_t j = i;
        while (j < files.size() && key(files[j]) == key(files[i])) ++j;
        const TileFile& f = files[i];
        if (component != std::tuple{f.dataset, f.selector1, f.selector2}) {
            component = {f.dataset, f.selector1, f.selector2};
            char code[32];
            std::snprintf(code, sizeof code, "D%03d S%03d T%03d", f.dataset, f.selector1,
                          f.selector2);
            std::cout << dataset_name(f.dataset).value_or("Dataset") << " (" << code << ")\n";
        }
        // CDB 1.3 Vol 1 §7.1.2.1 Req 14, §7.1.2.3 Req 16: one tile per geocell up to LOD 0,
        // then 2^LOD x 2^LOD.
        const std::int64_t total = tiles_per_geocell(f.tile.lod);
        const std::size_t n = j - i;
        partial = partial || std::int64_t(n) < total;
        std::cout << "  " << lod_name(f.tile.lod) << "  " << geocell_name(f.tile.geocell) << "  "
                  << n << " of " << total << " tiles" << (std::int64_t(n) < total ? "  partial" : "")
                  << "\n";
        i = j;
    }
    if (partial) std::cout << "partial: the store holds only some of the geocell's tiles at that LOD\n";
    return 0;
}

// Problems in the files under <cdb>/Tiles: paths that are not tile paths, and
// tiles the ogccdb-core readers reject. The readers apply the CDB rules on
// format, raster size, channels and data type.
int check(const fs::path& cdb, const std::vector<std::string>& args) {
    const TileQuery q = parse_query(args);
    const Store store = Store::open(cdb);
    const fs::path tiles = store.root() / "Tiles";
    std::vector<fs::path> paths;
    if (fs::exists(tiles))
        for (const auto& e : fs::recursive_directory_iterator(tiles))
            if (!e.is_directory()) paths.push_back(e.path());
    std::sort(paths.begin(), paths.end());

    std::size_t read = 0, skipped = 0, problems = 0;
    for (const fs::path& path : paths) {
        const std::string rel = path.lexically_relative(store.root()).generic_string();
        const auto parsed = parse_path(rel);
        if (!parsed || !parsed->root.empty()) {
            std::cout << rel << ": not a CDB tile path\n";
            ++problems;
            continue;
        }
        const TileFile& f = parsed->file;
        if ((q.dataset && f.dataset != *q.dataset) || (q.lod && f.tile.lod != *q.lod)) continue;
        try {
            const ImageryComponent c{f.selector1, f.selector2};
            if (f == elevation_file(f.tile)) store.read_elevation(f.tile);
            else if (f.dataset == kImagery && is_valid(c) && f == imagery_file(f.tile, c))
                store.read_imagery(f.tile, c);
            else {
                ++skipped;
                continue;
            }
            ++read;
        } catch (const std::exception& e) {
            // The readers prefix the message with the file's path.
            std::string why = e.what();
            const std::string prefix = path.string() + ": ";
            if (why.compare(0, prefix.size(), prefix) == 0) why.erase(0, prefix.size());
            std::cout << rel << ": " << why << "\n";
            ++problems;
        }
    }
    std::cout << read << " tiles read, " << problems << " problems";
    if (skipped) std::cout << ", " << skipped << " tiles of components not read";
    std::cout << "\n";
    return problems ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);
    try {
        if (args.size() == 1 && (args[0] == "-h" || args[0] == "--help")) {
            std::cout << kUsage;
            return 0;
        }
        if (args.size() < 2) throw UsageError("missing command or CDB folder");
        const std::vector<std::string> rest(args.begin() + 2, args.end());
        if (args[0] == "build") return build(args[1], rest);
        if (args[0] == "list") return list(args[1], rest);
        if (args[0] == "info") return info(args[1], rest);
        if (args[0] == "check") return check(args[1], rest);
        throw UsageError("unknown command " + args[0]);
    } catch (const UsageError& e) {
        std::cerr << "ogccdb-gen: " << e.what() << "\n" << kUsage;
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "ogccdb-gen: " << e.what() << "\n";
        return 1;
    }
}
