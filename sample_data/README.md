# Sample CDB from open data

`build_sample_cdb.py` builds a small OGC CDB 1.3 store with `ogccdb-gen` from an open
global 30 m DEM (Elevation) and open 10 m satellite imagery (Imagery). It reads only the
area of interest from the remote Cloud Optimized GeoTIFFs through GDAL, so a run
downloads about 150 MB for one geocell. Downloaded rasters and the generated store stay in
`out/sample/` (git-ignored); never commit them.

## Config

Copy `sources.template.json` to a file outside the repository and fill in:

- `area`: the area in degrees. Use whole geocells (1° × 1° at mid latitudes) to get every
  LOD; ogccdb-gen only imports tiles that overlap it (`--extent`).
- `elevation.urls`: DEM tiles in EPSG:4326 covering the area. Many 1° DEM tiles are
  shifted by half a post, so an area on a geocell's south or east edge also needs the
  neighbouring tiles.
- `imagery.urls`: one or more 8-bit RGB scenes in the same CRS (e.g. one UTM zone) that
  cover the area.
- `imagery.ignore_nodata`: some scenes store real black pixels (deep cloud shadow) with
  the nodata value 0, 0, 0, which makes ogccdb-gen skip their tiles. With `true`, the
  script checks that the area lies inside the scenes and then treats every pixel as data.
- `elevation.vertical_crs`: the vertical CRS of the DEM heights (EPSG code). Heights are
  converted to the WGS-84 ellipsoid, as CDB requires; PROJ needs that geoid grid
  (`projsync --file <grid>` or `PROJ_NETWORK=ON`). `--keep-geoid-heights` skips this.
- `*.attribution`: the credit lines the data licenses require. They are written into the
  `<Comment>` of the generated `Metadata/Version.xml`, so they travel with the data.

The default area is the whole geocell N47 E008 (8–9° E, 47–48° N), from two adjacent
scenes of one satellite pass and four DEM tiles.

## Run

Needs the GDAL and PROJ command line tools and a built `ogccdb-gen`:

```bash
python3 sample_data/build_sample_cdb.py --config ../sample-sources.json
```

Options: `--gen` (path to ogccdb-gen, default `build/ogccdb-gen`), `--work` (default
`out/sample`), `--force` (rebuild the CDB), `--min-lod` (default −10), `--max-lod`,
`--keep-geoid-heights`.

## What to expect

For the default geocell: 110 tiles, every tile of LOD −10 to 1 for Elevation (15) and of
LOD −10 to 3 for Imagery (95). The negative LODs, one tile each in folder `LC`, are part
of the sample because CDB 1.3 Vol 1 §7.1.2.2 Requirement 15 asks for the whole LOD
hierarchy down to LOD −10 in every geocell. The summary prints, per dataset, LOD and geocell, how many
of the geocell's tiles were written, and how much of each geocell the area covers.

For an area smaller than a geocell, the summary marks the geocell and its LODs as partial.
Where the sources cover only part of a tile, ogccdb-gen fills the rest with the dataset
default value (see the main README).
