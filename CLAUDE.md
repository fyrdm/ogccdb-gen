# ogccdb-gen — notes for Claude

C++17 command-line generator of OGC CDB 1.3 data stores (Elevation + Imagery from
GeoTIFF/DEM inputs). Uses the ogccdb-core library (repo fyrdm/ogccdb-core) as a
versioned vcpkg port from fyrdm/ogccdb-registry; no code is copied between repos.

## Rules
- CDB rules belong in ogccdb-core. This repo parses options, picks inputs and
  reports results. If a feature needs a CDB rule, add it to ogccdb-core first.
- Cite the spec for any CDB rule mentioned here: `CDB 1.3 Vol 1 §x.y`.
- Don't confuse OGC CDB 1.x with pre-OGC CDB 3.x or with CDB 2.0.
- This repo is public. Keep everything neutral: no company, vendor or commercial
  product names in code, comments, commit messages, PR titles and descriptions,
  docs or sample data. Open standards bodies (OGC, ISO, ...) may be named. Source
  URLs and credit lines for sample data live in a config file outside the repo.
- Small, focused PRs; tests must pass before opening one. Be concise.

## Build
```bash
cmake -B build -S . -G Ninja -DCMAKE_PREFIX_PATH=<ogccdb-core install prefix>
cmake --build build
ctest --test-dir build --output-on-failure
```
