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
ogccdb-gen info out/CDB          # tiles per dataset, LOD and geocell, e.g. "L03  N47E008  64 of 64 tiles"
ogccdb-gen check out/CDB         # reads every tile; one line per bad path or unreadable tile
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
- Tiles are written whole (CDB 1.3 Vol 1 §8.6.1.1). Where no input covers part of a
  tile, that part gets the dataset default value from `Metadata/Defaults.xml`, else
  the spec's value (Annex S): 0 m for Elevation, gray 0.5 for Imagery. `--no-fill`
  leaves such tiles out instead, together with the finer tiles under them
  (§7.1.2.2 Requirement 15). An input that reaches past a geocell edge also fills
  tiles of the neighbouring geocell; `--extent` keeps them out.

`sample_data/` builds a small sample store from open global data; see its README.

## Build

Needs CMake ≥ 3.20, a C++17 compiler and ogccdb-core 0.3.

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
