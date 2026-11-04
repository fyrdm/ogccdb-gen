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

#include "ogccdb/cdb13/store.hpp"

namespace fs = std::filesystem;
using namespace ogccdb::cdb13;

namespace {

constexpr std::string_view kUsage =
    "usage:\n"
    "  ogccdb-gen build <cdb> [--elevation <dem.tif|folder>]... [--imagery <image.tif|folder>]...\n"
    "                   [--min-lod N] [--max-lod N] [--resampling nearest|bilinear|cubic|average]\n"
    "                   [--quality 1..100] [--extent W,S,E,N] [--comment TEXT]\n"
    "  ogccdb-gen list <cdb> [--dataset 1|4] [--lod N]\n"
    "\n"
    "build creates the CDB Version <cdb> (Metadata/Version.xml) if it does not exist yet,\n"
    "then imports the Elevation and Imagery sources into it. Repeat --elevation or\n"
    "--imagery, or name a folder (its .tif files, by name), to mosaic several inputs of\n"
    "any size, resolution and CRS; where they overlap, the finer one wins (the later one\n"
    "at equal resolution). Without --max-lod, each input goes as fine as its resolution\n"
    "allows. --extent (degrees) imports\n"
    "only the tiles that overlap it, e.g. one geocell: --extent 8,47,9,48.\n"
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
    std::cout << what << ": " << r.written.size() << " tiles written, " << r.skipped.size()
              << " not fully covered\n";
}

int build(const fs::path& cdb, const std::vector<std::string>& args) {
    std::vector<fs::path> elevation, imagery;
    std::optional<std::string> comment;
    ImageryImportOptions opt;
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
        else if (flag == "--min-lod") opt.min_lod = to_int(flag, v);
        else if (flag == "--max-lod") opt.max_lod = to_int(flag, v);
        else if (flag == "--resampling") opt.resampling = to_resampling(v);
        else if (flag == "--quality") opt.encoding.quality = to_int(flag, v);
        else if (flag == "--extent") opt.extent = to_extent(v);
        else if (flag == "--comment") comment = v;
        else throw UsageError("unknown option " + flag);
    }

    const bool exists = fs::exists(cdb / fs::path(std::string(kVersionFile)));
    if (exists && comment) throw UsageError("--comment only applies to a new CDB");
    VersionMetadata version;
    version.comment = comment;
    Store store = exists ? Store::open(cdb) : Store::create(cdb, version);
    std::cout << (exists ? "opened " : "created ") << store.root().string() << "\n";

    if (!elevation.empty()) report("Elevation", store.import_elevation(elevation, opt));
    if (!imagery.empty()) report("Imagery", store.import_imagery(imagery, opt));
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
