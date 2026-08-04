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

This repository contains an experimental DuckDB extension for reading and
writing Apache TsFile table-model files directly with SQL:

```sql
SELECT * FROM read_tsfile('/data/measurements.tsfile', 'sensors');
```

The current prototype targets DuckDB `v1.5.5` and Apache TsFile commit
`7ca2e79fbdd9a36ef9dde5e522b7c23020536aeb`. Both dependencies are pinned as
Git submodules.

## Status

The extension supports:

- one local TsFile and one table per `read_tsfile(path, table_name)` call;
- DuckDB projection pushdown into the TsFile column list;
- `time` predicates using `=`, `<`, `<=`, `>`, `>=`, and `BETWEEN`;
- TAG predicates using `=`, `!=`, `<`, `<=`, `>`, `>=`, `BETWEEN`,
  `IS NULL`, and `IS NOT NULL`, including supported `AND`/`OR` combinations;
- BOOLEAN, INT32, INT64, FLOAT, DOUBLE, TEXT, STRING, TIMESTAMP, DATE, and BLOB;
- NULL values and result sets spanning multiple DuckDB vector batches.

The global TsFile time axis is exposed as `BIGINT` because its unit is a file
or protocol convention. A TsFile `TIMESTAMP` measurement is exposed as DuckDB
`TIMESTAMP_NS`, matching the Arrow C schema produced by the TsFile C wrapper.

Tree-model files, multi-file scans, automatic table discovery, parallel scans,
FIELD filter pushdown, arbitrary `NOT` TAG expressions, and zero-copy Arrow
transfer are not implemented. Unsupported filters remain in DuckDB and are
evaluated after the scan.

## Build

Clone this repository with all of its build dependencies:

```shell
git clone --recurse-submodules git@github.com:ColinLeeo/tsfile-duckdb.git
```

Then build the extension:

```shell
cd /path/to/tsfile-duckdb
GEN=ninja make
```

The build compiles the pinned TsFile sources and bundled codecs as
position-independent static libraries in an isolated CMake build. The
resulting extension does not require a separately installed `libtsfile`.

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
  -DUNITTEST_ROOT_DIRECTORY="$PWD" \
  -DBUILD_UNITTESTS=ON
cmake --build build/release --target duckdb tsfile_loadable_extension unittest -j8
```

## Usage

Start the locally built DuckDB shell with unsigned extensions enabled, then
load the extension:

```shell
build/release/duckdb -unsigned
```

```sql
LOAD 'build/release/extension/tsfile/tsfile.duckdb_extension';
```

### Read and query a TsFile

`read_tsfile` exposes one table-model TsFile table as a DuckDB relation. The
second argument is the table name stored in the TsFile:

```sql
SELECT time, device_id, temperature
FROM read_tsfile('/data/measurements.tsfile', 'sensors')
LIMIT 10;
```

Projection, TAG equality/range predicates, and time predicates are pushed into
the TsFile scan when they match the supported forms:

```sql
SELECT time, device_id, temperature
FROM read_tsfile('/data/measurements.tsfile', 'sensors')
WHERE device_id = 'device-01'
  AND time BETWEEN 1700000000000 AND 1700003600000;
```

Use `EXPLAIN` to inspect the pushed `Time Range` and `TAG Filter`:

```sql
EXPLAIN
SELECT time, device_id, temperature
FROM read_tsfile('/data/measurements.tsfile', 'sensors')
WHERE device_id = 'device-01'
  AND time >= 1700000000000;
```

### Write a TsFile with COPY

The writer uses DuckDB's standard `COPY` interface. This complete example
creates a small source relation, writes it, and reads it back:

```sql
CREATE TABLE measurements AS
SELECT * FROM (VALUES
    (1700000000000::BIGINT, 'device-01'::VARCHAR, 21.5::DOUBLE, 40.1::DOUBLE),
    (1700000001000::BIGINT, 'device-01'::VARCHAR, 21.7::DOUBLE, 40.0::DOUBLE),
    (1700000000000::BIGINT, 'device-02'::VARCHAR, 19.8::DOUBLE, 45.2::DOUBLE)
) t(time, device_id, temperature, humidity);

COPY (
    SELECT time, device_id, temperature, humidity
    FROM measurements
    ORDER BY device_id, time
)
TO '/tmp/measurements.tsfile'
(
    FORMAT tsfile,
    TABLE_NAME sensors,
    TIME_COLUMN time,
    TAG_COLUMNS (device_id)
);

SELECT *
FROM read_tsfile('/tmp/measurements.tsfile', 'sensors')
ORDER BY device_id, time;
```

`TIME_COLUMN` identifies the `BIGINT` time axis. Columns listed in
`TAG_COLUMNS` must be fixed `VARCHAR` columns; every other input column becomes
a FIELD measurement. `TABLE_NAME` and `TIME_COLUMN` are documented as
identifiers, while their single-quoted string forms remain accepted for
compatibility. Use double-quoted identifiers when a name needs quoting.

For an existing output path, leave DuckDB's temporary-file handling enabled
(the default) and use `OVERWRITE true` if replacement is intended:

```sql
COPY (SELECT * FROM measurements ORDER BY device_id, time)
TO '/tmp/measurements.tsfile'
(
    FORMAT tsfile,
    TABLE_NAME sensors,
    TIME_COLUMN time,
    TAG_COLUMNS (device_id),
    OVERWRITE true
);
```

### Current writer limitations

The first writer implementation creates one local table per file, requires a
`BIGINT` time column, and preserves NULLs in FIELD columns. TAG and TIME values
must not be NULL. Input should be ordered by the TAG columns followed by time.
DATE FIELD writing is temporarily disabled until the TsFile DATE conversion is
timezone-independent; DATE values can still be read from existing TsFiles.
Direct writes with `USE_TMP_FILE false` require a new target path; use the
default temporary-file handling with `OVERWRITE true` to replace an existing
file.

See [USAGE.md](USAGE.md) for the complete read, query, write, and round-trip
workflow.

## Tests

```shell
GEN=ninja make test
```

The checked-in fixture is documented in `test/data/README.md`.

## Community extension roadmap

The extension now builds a pinned Apache TsFile revision in its own CMake
project and statically links TsFile and its bundled codecs. This keeps TsFile's
global CMake settings out of DuckDB and produces a self-contained extension
artifact. The next integration step is to validate the supported DuckDB target
matrix and submit an extension descriptor to the DuckDB community extension
repository.
