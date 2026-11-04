# ogccdb-gen

Command-line generator of [OGC CDB](https://www.ogc.org/standard/cdb/) 1.3 data
stores. It cuts GeoTIFF/DEM and imagery inputs into CDB geocells, tiles and LODs
(Elevation and Imagery datasets) and writes the CDB metadata. Built on the
ogccdb-core C++17 library. License: MIT.

> Status: early development.

## Use

```bash
ogccdb-gen build out/CDB --elevation dem_tiles/ --imagery scene1.tif --imagery drone.tif
ogccdb-gen list out/CDB --dataset 4 --lod 3
ogccdb-gen --help
```

- Inputs can be of any size, resolution and CRS that GDAL can transform to WGS-84.
  Repeat `--elevation`/`--imagery`, or name a folder to take its `.tif` files.
- Each input is written down to the finest LOD its resolution allows. Where inputs
  overlap, the finer one wins (the later one at equal resolution), and coarser ones
  fill the rest of a tile.
- LODs are picked per dataset: from LOD −10 down to the finest LOD the inputs'
  resolution allows. `--min-lod`, `--max-lod` and `--resampling` set both datasets;
  `--elevation-min-lod`, `--imagery-max-lod`, `--imagery-resampling` and so on set one.
- `--extent W,S,E,N` (degrees) limits the import, e.g. to one geocell.

`sample_data/` builds a small sample store from open global data; see its README.

## Build

Needs CMake ≥ 3.20, a C++17 compiler and ogccdb-core 0.2.

With ogccdb-core installed to a prefix (it needs GDAL, pugixml and, for the tests,
GTest from system packages):

```bash
cmake -B build -S . -G Ninja -DCMAKE_PREFIX_PATH=/path/to/ogccdb-core-prefix
cmake --build build
ctest --test-dir build --output-on-failure
```

With vcpkg, `vcpkg.json` lists the dependencies and `vcpkg-configuration.json` takes
ogccdb-core from the project's vcpkg registry (fyrdm/ogccdb-registry). While that
registry and ogccdb-core are private, git must be able to read them
(`git ls-remote https://github.com/fyrdm/ogccdb-core.git` works without asking):

```bash
cmake -B build -S . -G Ninja -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
cmake --build build
ctest --test-dir build --output-on-failure
```

To use a newer ogccdb-core, move the registry `baseline` to a later commit of the
registry.
