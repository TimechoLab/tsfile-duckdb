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

# DuckDB TsFile extension usage

This extension provides both sides of a table-model TsFile workflow:

```text
TsFile -> read_tsfile -> DuckDB SQL -> COPY FORMAT tsfile -> TsFile
```

The examples below use the locally built extension. The same SQL applies to
the packaged community extension once it is available.

## Load the extension

The local build produces an unsigned loadable extension. Start DuckDB with
`-unsigned` and load it explicitly:

```shell
build/release/duckdb -unsigned
```

```sql
LOAD 'build/release/extension/tsfile/tsfile.duckdb_extension';
```

## Read a TsFile

Use `read_tsfile(path, table_name)` as a table function:

```sql
SELECT *
FROM read_tsfile('/data/measurements.tsfile', 'sensors');
```

Projection, time predicates, and TAG predicates can be written as normal SQL.
Time predicates are pushed into the TsFile scan when they use `=`, `<`, `<=`,
`>`, `>=`, or `BETWEEN`:

```sql
SELECT time, device_id, temperature
FROM read_tsfile('/data/measurements.tsfile', 'sensors')
WHERE time BETWEEN 1700000000000 AND 1700003600000;
```

String TAG columns are pushed into TsFile's device filter when they use `=`,
`!=`, `<`, `<=`, `>`, `>=`, inclusive `BETWEEN`, `IS NULL`, or `IS NOT NULL`.
Supported TAG predicates can also be combined with `AND` and `OR`:

```sql
SELECT time, device_id, temperature
FROM read_tsfile('/data/measurements.tsfile', 'sensors')
WHERE (device_id = 'device-01' OR device_id = 'device-02')
  AND time BETWEEN 1700000000000 AND 1700003600000;
```

Use `EXPLAIN` to verify the scan. Pushed predicates appear as `Time Range` and
`TAG Filter` properties of `READ_TSFILE`. FIELD predicates and unsupported TAG
expressions, including arbitrary `NOT (...)`, remain in DuckDB and are evaluated
after the scan.

The global TsFile time axis is exposed as DuckDB `BIGINT`. A TsFile timestamp
measurement is exposed as DuckDB `TIMESTAMP_NS`.

## Write a TsFile

Use DuckDB's standard `COPY` command with `FORMAT tsfile`:

```sql
COPY (
    SELECT time, device_id, temperature, humidity
    FROM measurements
    ORDER BY device_id, time
)
TO '/data/measurements.tsfile'
(
    FORMAT tsfile,
    TABLE_NAME sensors,
    TIME_COLUMN time,
    TAG_COLUMNS (device_id)
);
```

The write options are:

| Option | Required | Meaning |
| --- | --- | --- |
| `FORMAT tsfile` | Yes | Selects the TsFile COPY writer. |
| `TABLE_NAME` | No | Local table name in the output file. Defaults to `default_table`. |
| `TIME_COLUMN` | No | Input column used as the time axis. Defaults to `time`. It must be `BIGINT`. |
| `TAG_COLUMNS` | No | Input columns used as TAG columns. Omitted means every non-time column is a FIELD. |

All input columns except `TIME_COLUMN` and the columns listed in
`TAG_COLUMNS` become FIELD measurements.

`TABLE_NAME` and `TIME_COLUMN` are documented using identifier syntax. Their
single-quoted string forms remain accepted for compatibility, but the
identifier form makes it clear that `TIME_COLUMN` names an input column:

```sql
TABLE_NAME sensors,
TIME_COLUMN event_time,
TAG_COLUMNS (device_id)
```

## TAG_COLUMNS

`TAG_COLUMNS` takes a column-name list, not a list of values:

```sql
TAG_COLUMNS (device_id, region)
```

TAG columns must:

- exist in the input query;
- have DuckDB type `VARCHAR`;
- contain no NULL values;
- not be the `TIME_COLUMN`.

Column matching is case-insensitive. If `TAG_COLUMNS` is omitted, the output
file has no TAG columns:

```sql
COPY (
    SELECT time, temperature, humidity
    FROM measurements
    ORDER BY time
)
TO '/data/field-only.tsfile'
(
    FORMAT tsfile,
    TABLE_NAME sensors,
    TIME_COLUMN time
);
```

## Supported write types

The current writer maps these DuckDB types to TsFile types:

| DuckDB type | TsFile type | NULL handling |
| --- | --- | --- |
| `BOOLEAN` | BOOLEAN | FIELD NULL is preserved. |
| `INTEGER` | INT32 | FIELD NULL is preserved. |
| `BIGINT` | INT64 | FIELD NULL is preserved. `TIME_COLUMN` also uses this type. |
| `FLOAT` | FLOAT | FIELD NULL is preserved. |
| `DOUBLE` | DOUBLE | FIELD NULL is preserved. |
| `VARCHAR` | STRING | FIELD NULL is preserved; TAG NULL is rejected. |
| `BLOB` | BLOB | FIELD NULL is preserved. |
| `TIMESTAMP_NS` | TIMESTAMP | FIELD NULL is preserved. |

## Complete read-query-write-read pipeline

The following example reads the repository fixture, selects a time range,
writes the selected rows to a new TsFile, and reads the result back:

```sql
LOAD 'build/release/extension/tsfile/tsfile.duckdb_extension';

COPY (
    SELECT time,
           s0 AS device_id,
           s2 AS value,
           s8 AS day
    FROM read_tsfile('test/data/simple_table_t1.tsfile', 'test')
    WHERE s0 = 'a'
      AND time BETWEEN 1760106022000 AND 1760106024000
    ORDER BY device_id, time
)
TO '/tmp/tsfile_subset.tsfile'
(
    FORMAT tsfile,
    TABLE_NAME subset,
    TIME_COLUMN time,
    TAG_COLUMNS (device_id)
);

SELECT time, device_id, value, day
FROM read_tsfile('/tmp/tsfile_subset.tsfile', 'subset')
ORDER BY device_id, time;
```

The checked-in fixture produces three rows:

```text
time             device_id  value  day
1760106022000    a          1012   2024-10-22
1760106023000    a          1013   2024-10-23
1760106024000    a          1014   2024-10-24
```

## Ordering and current limitations

Before writing, order the input by all TAG columns and then by `TIME_COLUMN`:

```sql
ORDER BY device_id, region, time
```

The first writer implementation currently has these limitations:

- `TIME_COLUMN` must be a `BIGINT`.
- DATE FIELD writing is temporarily unsupported because the pinned TsFile DATE
  conversion depends on the local timezone. Existing DATE measurements remain
  readable.
- Direct writes with `USE_TMP_FILE false` require a new target path. To replace
  an existing file, leave temporary-file handling enabled (the default).
- TAG and TIME values cannot be NULL.
- Input should be ordered by TAG columns followed by time.
- One output file contains one local table created by the writer.
- Append mode and parallel writing are not implemented yet.
- Tree-model TsFiles are not supported by this table-model extension.

## Build and test

Build the pinned TsFile dependency first, then build this extension as
described in [README.md](README.md#build). Run the SQL tests with:

```shell
export TSFILE_ROOT=/path/to/tsfile
GEN=ninja make test
```
