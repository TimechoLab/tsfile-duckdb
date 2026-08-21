<!--

    Licensed to the Apache Software Foundation (ASF) under one
    or more contributor license agreements.  See the NOTICE file
    distributed with this work for additional information
    regarding copyright ownership.  The ASF licenses this file
    to you under the Apache License, Version 2.0 (the
    "License"); you may not use this file except in compliance
    with the License.  You may obtain a copy of the License at

        http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing,
    software distributed under the License is distributed on an
    "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
    KIND, either express or implied.  See the License for the
    specific language governing permissions and limitations
    under the License.

-->

# DuckDB TsFile extension

This repository contains an experimental DuckDB extension for querying Apache
TsFile table-model files directly with SQL:

```sql
SELECT * FROM read_tsfile('/data/measurements.tsfile', 'sensors');
```

The current prototype targets DuckDB `v1.5.5` and the Apache TsFile C wrapper
at commit `1bdbcd857d058ffda7f2d73a26b212504701deb4`. That TsFile commit is
available from the `fix/aligned-gorilla-nan-parallel-read` branch of
[`ColinLeeo/tsfile`](https://github.com/ColinLeeo/tsfile).

## Status

The extension supports:

- one local TsFile and one table per `read_tsfile(path, table_name)` call;
- DuckDB projection pushdown into the TsFile column list;
- `time` predicates using `=`, `<`, `<=`, `>`, `>=`, and `BETWEEN`;
- BOOLEAN, INT32, INT64, FLOAT, DOUBLE, TEXT, STRING, TIMESTAMP, DATE, and BLOB;
- NULL values and result sets spanning multiple DuckDB vector batches.

The global TsFile time axis is exposed as `BIGINT` because its unit is a file
or protocol convention. A TsFile `TIMESTAMP` measurement is exposed as DuckDB
`TIMESTAMP_NS`, matching the Arrow C schema produced by the TsFile C wrapper.

Tree-model files, multi-file scans, automatic table discovery, parallel scans,
TAG/FIELD filter pushdown, and zero-copy Arrow transfer are not implemented.
Unsupported filters remain in DuckDB and are evaluated after the scan.

## Build

Clone this repository with its DuckDB build submodules:

```shell
git clone --recurse-submodules git@github.com:ColinLeeo/tsfile-duckdb.git
```

Build the required TsFile C++ library from the pinned fork revision:

```shell
git clone https://github.com/ColinLeeo/tsfile.git
cd tsfile
git checkout 1bdbcd857d058ffda7f2d73a26b212504701deb4
cd cpp
bash build.sh -t=Release --disable-antlr4
```

Then build the extension:

```shell
cd /path/to/tsfile-duckdb
export TSFILE_ROOT=/path/to/tsfile
GEN=ninja make
```

The main outputs are:

```text
build/release/duckdb
build/release/test/unittest
build/release/extension/tsfile/tsfile.duckdb_extension
```

On macOS, if DuckDB platform probing stalls, configure explicitly:

```shell
cmake -S duckdb -B build/release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DDUCKDB_EXPLICIT_PLATFORM=osx_arm64 \
  -DDUCKDB_EXTENSION_CONFIGS="$PWD/extension_config.cmake" \
  -DTSFILE_BUILD_DIR="$TSFILE_ROOT/cpp/build/Release" \
  -DBUILD_UNITTESTS=ON
cmake --build build/release --target duckdb tsfile_loadable_extension unittest -j8
```

## Query

The locally built loadable extension is unsigned:

```shell
build/release/duckdb -unsigned
```

```sql
LOAD 'build/release/extension/tsfile/tsfile.duckdb_extension';

SELECT time, device_id, temperature
FROM read_tsfile('/data/measurements.tsfile', 'sensors')
WHERE time BETWEEN 1700000000000 AND 1700003600000;
```

`EXPLAIN` displays a pushed time range on the `READ_TSFILE` operator.

To write a table-model TsFile, use DuckDB's standard `COPY` interface. The
`TIME_COLUMN` is written as the TsFile time axis, columns listed in
`TAG_COLUMNS` must be `VARCHAR` and identify the table's device, and all other
columns become FIELD measurements:

```sql
COPY (
    SELECT time, device_id, temperature, humidity
    FROM measurements
    ORDER BY device_id, time
)
TO '/data/measurements.tsfile'
(
    FORMAT tsfile,
    TABLE_NAME 'sensors',
    TIME_COLUMN 'time',
    TAG_COLUMNS (device_id)
);
```

The first writer implementation creates one local table per file, requires a
`BIGINT` time column, and preserves NULLs in FIELD columns. TAG and TIME values
must not be NULL. Input should be ordered by the TAG columns followed by time.

See [USAGE.md](USAGE.md) for the complete read, query, write, and round-trip
workflow.

## Tests

```shell
export TSFILE_ROOT=/path/to/tsfile
GEN=ninja make test
```

The checked-in fixture is documented in `test/data/README.md`.

## Community extension roadmap

The prototype currently links a prebuilt shared `libtsfile`. Before submission
to DuckDB's community extension repository, TsFile must be built reproducibly
inside the extension pipeline and linked or packaged portably for every DuckDB
target. The next integration step is to add a pinned dependency build that does
not leak TsFile's global CMake flags into DuckDB and produces a self-contained
extension artifact.
