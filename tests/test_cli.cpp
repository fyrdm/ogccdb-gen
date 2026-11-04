#include <gdal_priv.h>
#include <gtest/gtest.h>
#include <ogr_spatialref.h>

#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include "ogccdb/cdb13/lod.hpp"
#include "ogccdb/cdb13/store.hpp"

using namespace ogccdb::cdb13;
namespace fs = std::filesystem;

namespace {

// Synthetic inputs over N10E010: a tilted plane for Elevation and gentle
// ramps for Imagery, which bilinear resampling reproduces (see the import tests).
double plane(double lat, double lon) { return 1000.0 + 200.0 * lat - 100.0 * lon; }
std::array<double, 3> rgb(double lat, double lon) {
    return {40.0 + 100.0 * (lon - 9.75), 30.0 + 120.0 * (lat - 9.75), 200.0 - 50.0 * (lon - 9.75)};
}

// 0.0025 degree pixels over [9.75, 11.25]: covers N10E010 with a margin, and
// the default finest LOD is -2 (256 posts per degree, not finer than 400).
constexpr int kSize = 600;
constexpr double kPx = 1.5 / kSize;
constexpr int kFinestLod = -2;
const Geocell kCell{10, 10};

// Text mode, so the generator's output reads with '\n' line ends on Windows too.
std::string read_text(const fs::path& p) {
    std::ifstream in(p);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

class GenTest : public ::testing::Test {
protected:
    void SetUp() override {
        GDALAllRegister();
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        dir_ = fs::temp_directory_path() / (std::string("ogccdb_gen_") + info->name());
        fs::remove_all(dir_);
        fs::create_directories(dir_ / "src");
        root_ = dir_ / "cdb";
    }
    void TearDown() override { fs::remove_all(dir_); }

    // Writes a kSize x kSize GeoTIFF in EPSG:4326 over [9.75, 11.25]^2 with
    // value(lon, lat) of each band at the pixel centres.
    fs::path make_source(const std::string& name, GDALDataType type, const std::vector<GDALColorInterp>& interp,
                         const std::function<std::vector<double>(double, double)>& value) {
        const fs::path path = dir_ / "src" / name;
        const int nb = int(interp.size());
        auto* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
        GDALDataset* ds = drv->Create(path.string().c_str(), kSize, kSize, nb, type, nullptr);
        EXPECT_NE(ds, nullptr);
        OGRSpatialReference srs;
        srs.importFromEPSG(4326);
        ds->SetSpatialRef(&srs);
        std::array<double, 6> gt = {9.75, kPx, 0, 11.25, 0, -kPx};
        ds->SetGeoTransform(gt.data());
        std::vector<std::vector<float>> bands(nb, std::vector<float>(std::size_t(kSize) * kSize));
        for (int j = 0; j < kSize; ++j)
            for (int i = 0; i < kSize; ++i) {
                const auto v = value(gt[0] + (i + 0.5) * gt[1], gt[3] + (j + 0.5) * gt[5]);
                for (int b = 0; b < nb; ++b)
                    bands[b][std::size_t(j) * kSize + i] = float(type == GDT_Byte ? std::round(v[b]) : v[b]);
            }
        for (int b = 0; b < nb; ++b) {
            GDALRasterBand* band = ds->GetRasterBand(b + 1);
            band->SetColorInterpretation(interp[b]);
            EXPECT_EQ(band->RasterIO(GF_Write, 0, 0, kSize, kSize, bands[b].data(), kSize, kSize, GDT_Float32,
                                     0, 0, nullptr),
                      CE_None);
        }
        GDALClose(ds);
        return path;
    }
    fs::path make_dem() {
        return make_source("dem.tif", GDT_Float32, {GCI_GrayIndex},
                           [](double lon, double lat) { return std::vector<double>{plane(lat, lon)}; });
    }
    fs::path make_rgb() {
        return make_source("rgb.tif", GDT_Byte, {GCI_RedBand, GCI_GreenBand, GCI_BlueBand},
                           [](double lon, double lat) {
                               const auto c = rgb(lat, lon);
                               return std::vector<double>(c.begin(), c.end());
                           });
    }

    // Checks a store built from make_dem() and make_rgb() with default options:
    // one tile of N10E010 per dataset and LOD -10..-2, holding the inputs.
    void expect_built(const Store& store) {
        const std::size_t lods = kFinestLod - kMinLod + 1;
        // The single LOD -10 post of a geocell sits on its north-west corner, so
        // the source also covers N09E010, N09E011 and N10E011 at that LOD.
        EXPECT_EQ(store.list_tiles().size(), 2 * lods + 3);
        EXPECT_EQ(store.list_tiles({kElevation, kMinLod, std::nullopt}).size(), 4u);
        const auto all = store.list_tiles({std::nullopt, std::nullopt, extent(kCell)});
        ASSERT_EQ(all.size(), 2 * lods);
        for (std::size_t k = 0; k < all.size(); ++k) {
            const bool elevation = k < lods;
            const int lod = kMinLod + int(k % lods);
            EXPECT_EQ(all[k], elevation ? elevation_file({kCell, lod, 0, 0}) : imagery_file({kCell, lod, 0, 0}))
                << relative_path(all[k]);
        }

        const TileAddress finest{kCell, kFinestLod, 0, 0};
        const int n = raster_size(kFinestLod);
        const auto dem = store.read_elevation(finest);
        ASSERT_TRUE(dem);
        const auto img = store.read_imagery(finest);
        ASSERT_TRUE(img);
        ASSERT_EQ(img->channels, 3);
        for (int j : {0, n / 2, n - 1})
            for (int i : {0, n / 2, n - 1}) {
                const LatLon p = grid_element_position(finest, kElevationAlignment, i, j);
                EXPECT_NEAR(dem->elevation[std::size_t(j) * n + i], plane(p.lat, p.lon), 0.05);
                const LatLon q = grid_element_position(finest, kImageryAlignment, i, j);
                const auto want = rgb(q.lat, q.lon);
                for (int c = 0; c < 3; ++c)
                    EXPECT_NEAR(img->pixels[(std::size_t(j) * n + i) * 3 + c], want[c], 1.5);
            }
    }

    // Runs ogccdb-gen with `args`; its standard output goes to out().
    int run(const std::string& args) const {
        std::string cmd = "\"" + std::string(OGCCDB_GEN_EXE) + "\" " + args + " > \"" + out().string() + "\"";
#ifdef _WIN32
        cmd = "\"" + cmd + "\"";  // cmd.exe strips the outer quotes
#endif
        return std::system(cmd.c_str());
    }
    fs::path out() const { return dir_ / "out.txt"; }

    fs::path dir_, root_;
};

}  // namespace

// ogccdb-gen builds a store from the inputs and lists its tiles.
TEST_F(GenTest, BuildAndList) {
    const std::string cdb = "\"" + root_.string() + "\"";
    // Imagery from a folder of scenes, here a single one.
    const fs::path scenes = dir_ / "scenes";
    fs::create_directories(scenes);
    fs::rename(make_rgb(), scenes / "rgb.tif");
    ASSERT_EQ(run("build " + cdb + " --elevation \"" + make_dem().string() + "\" --imagery \"" +
                  scenes.string() + "\" --comment example"),
              0)
        << read_text(out());
    const Store store = Store::open(root_);
    EXPECT_EQ(store.version().comment, "example");
    expect_built(store);

    ASSERT_EQ(run("list " + cdb + " --dataset 4 --lod -2"), 0);
    EXPECT_EQ(read_text(out()), relative_path(imagery_file({kCell, -2, 0, 0})) + "\n");
    EXPECT_NE(run("list \"" + (dir_ / "missing").string() + "\""), 0);
    EXPECT_NE(run("build " + cdb + " --max-lod x"), 0);
    fs::create_directories(dir_ / "empty");
    EXPECT_NE(run("build " + cdb + " --imagery \"" + (dir_ / "empty").string() + "\""), 0);
}

// Each dataset takes its own LOD range: shared options apply to both, the
// --elevation-* and --imagery-* forms override them for one dataset.
TEST_F(GenTest, OptionsPerDataset) {
    const std::string cdb = "\"" + root_.string() + "\"";
    ASSERT_EQ(run("build " + cdb + " --elevation \"" + make_dem().string() + "\" --imagery \"" +
                  make_rgb().string() + "\" --min-lod -4 --elevation-max-lod -3" +
                  " --imagery-min-lod -2 --imagery-resampling average --extent 10,10,11,11"),
              0)
        << read_text(out());
    EXPECT_NE(read_text(out()).find("Elevation: 2 tiles written (LC04 to LC03)"), std::string::npos)
        << read_text(out());
    EXPECT_NE(read_text(out()).find("Imagery: 1 tiles written (LC02 to LC02)"), std::string::npos)
        << read_text(out());
    const Store store = Store::open(root_);
    EXPECT_EQ(store.list_tiles({kElevation, std::nullopt, std::nullopt}).size(), 2u);
    EXPECT_EQ(store.list_tiles({kImagery, std::nullopt, std::nullopt}).size(), 1u);
    EXPECT_NE(run("build " + cdb + " --imagery-resampling sharp"), 0);
}
