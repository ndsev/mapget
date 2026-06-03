#!/usr/bin/env python3
"""Build the bundled GeoNames cities5000 SQLite lookup database."""

from __future__ import annotations

import os
import sqlite3
import sys
from pathlib import Path


SCHEMA = """
PRAGMA journal_mode = OFF;
PRAGMA synchronous = OFF;
PRAGMA temp_store = MEMORY;
CREATE TABLE location (
  geoname_id INTEGER PRIMARY KEY,
  name TEXT NOT NULL,
  ascii_name TEXT NOT NULL,
  alternate_names TEXT,
  latitude REAL NOT NULL,
  longitude REAL NOT NULL,
  feature_class TEXT NOT NULL,
  feature_code TEXT NOT NULL,
  country_code TEXT NOT NULL,
  admin1_code TEXT,
  admin2_code TEXT,
  population INTEGER,
  elevation INTEGER,
  timezone TEXT,
  modification_date TEXT
);
CREATE INDEX location_country_idx ON location(country_code);
CREATE INDEX location_population_idx ON location(population DESC);
CREATE VIRTUAL TABLE location_fts USING fts5(
  name,
  ascii_name,
  alternate_names,
  content='location',
  content_rowid='geoname_id',
  tokenize='unicode61 remove_diacritics 2'
);
"""


INSERT_LOCATION = """
INSERT INTO location (
  geoname_id,
  name,
  ascii_name,
  alternate_names,
  latitude,
  longitude,
  feature_class,
  feature_code,
  country_code,
  admin1_code,
  admin2_code,
  population,
  elevation,
  timezone,
  modification_date
) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
"""


POPULATE_FTS = """
INSERT INTO location_fts(rowid, name, ascii_name, alternate_names)
SELECT geoname_id, name, ascii_name, COALESCE(alternate_names, '')
FROM location
"""


def text_or_none(value: str) -> str | None:
    """Convert empty GeoNames text columns to SQLite NULL."""
    return value or None


def int_or_none(value: str) -> int | None:
    """Convert empty GeoNames integer columns to SQLite NULL."""
    return int(value) if value else None


def import_geonames(input_txt: Path, output_sqlite: Path) -> tuple[int, int]:
    """Import GeoNames rows into a fresh SQLite database and return read/imported counts."""
    output_sqlite.parent.mkdir(parents=True, exist_ok=True)
    temp_sqlite = output_sqlite.with_name(output_sqlite.name + ".tmp")
    temp_sqlite.unlink(missing_ok=True)

    rows_read = 0
    rows_imported = 0
    con = sqlite3.connect(temp_sqlite)
    try:
        con.executescript(SCHEMA)
        with con:
            with input_txt.open("r", encoding="utf-8", newline="") as input_file:
                for rows_read, line in enumerate(input_file, start=1):
                    line = line.rstrip("\r\n")
                    columns = line.split("\t")
                    if len(columns) != 19:
                        raise ValueError(f"GeoNames row does not have 19 columns at line {rows_read}")

                    con.execute(
                        INSERT_LOCATION,
                        (
                            int(columns[0]),
                            columns[1],
                            columns[2] or columns[1],
                            text_or_none(columns[3]),
                            float(columns[4]),
                            float(columns[5]),
                            columns[6],
                            columns[7],
                            columns[8],
                            text_or_none(columns[10]),
                            text_or_none(columns[11]),
                            int_or_none(columns[14]),
                            int_or_none(columns[15]),
                            text_or_none(columns[17]),
                            text_or_none(columns[18]),
                        ),
                    )
                    rows_imported += 1
            con.execute(POPULATE_FTS)
        con.close()
        os.replace(temp_sqlite, output_sqlite)
        return rows_read, rows_imported
    except Exception:
        con.rollback()
        con.close()
        temp_sqlite.unlink(missing_ok=True)
        raise


def main(argv: list[str]) -> int:
    """Run the build-time GeoNames importer command."""
    if len(argv) != 3:
        print("Usage: import_geonames.py <cities5000.txt> <geonames-cities5000.sqlite>", file=sys.stderr)
        return 2

    try:
        rows_read, rows_imported = import_geonames(Path(argv[1]), Path(argv[2]))
    except Exception as exc:
        print(f"GeoNames import failed: {exc}", file=sys.stderr)
        return 1

    print(f"Imported {rows_imported} of {rows_read} GeoNames rows into {argv[2]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
