// ogccdb-gen: builds an OGC CDB 1.3 data store from GeoTIFF/DEM and RGB
// GeoTIFF inputs, and lists its tiles. Built on the ogccdb-core library.

#include <stdexcept>
#include <algorithm>
#include <cctype>
#include <exception>
#include <filesystem>
#include <iostream>
#include <locale>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
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
    "                   [--quality 1..100] [--extent W,S,E,N] [--comment TEXT]\n"
    "  ogccdb-gen list <cdb> [--dataset 1|4] [--lod N]\n"
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
    "list prints the tile files of <cdb>, one path per line.\n";

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

void report(std::string_view what, const RasterImportResult& r) {
    std::cout << what << ": " << r.written.size() << " tiles written";
    if (!r.written.empty()) {
        const auto [lo, hi] = std::minmax_element(
            r.written.begin(), r.written.end(),
            [](const TileAddress& a, const TileAddress& b) { return a.lod < b.lod; });
        std::cout << " (" << lod_name(lo->lod) << " to " << lod_name(hi->lod) << ")";
    }
    std::cout << ", " << r.skipped.size() << " not fully covered\n";
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
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& flag = args[i];
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
    apply(both, elevation_opt);
    apply(elevation_only, elevation_opt);
    apply(both, imagery_opt);
    apply(imagery_only, imagery_opt);
    if (!elevation.empty()) report("Elevation", store.import_elevation(elevation, elevation_opt));
    if (!imagery.empty()) report("Imagery", store.import_imagery(imagery, imagery_opt));
    return 0;
}

int list(const fs::path& cdb, const std::vector<std::string>& args) {
    TileQuery q;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& flag = args[i];
        if (i + 1 >= args.size()) throw UsageError(flag + " needs a value");
        const std::string& v = args[++i];
        if (flag == "--dataset") q.dataset = to_int(flag, v);
        else if (flag == "--lod") q.lod = to_int(flag, v);
        else throw UsageError("unknown option " + flag);
    }
    const Store store = Store::open(cdb);
    for (const TileFile& f : store.list_tiles(q)) std::cout << relative_path(f) << "\n";
    return 0;
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
        throw UsageError("unknown command " + args[0]);
    } catch (const UsageError& e) {
        std::cerr << "ogccdb-gen: " << e.what() << "\n" << kUsage;
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "ogccdb-gen: " << e.what() << "\n";
        return 1;
    }
}
