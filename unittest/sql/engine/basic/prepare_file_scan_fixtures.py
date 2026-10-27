#!/usr/bin/env python3
# Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
"""Decode the deterministic Parquet inputs for test_file_scan_utils."""
import base64
import pathlib
import sys

here = pathlib.Path(__file__).resolve().parent
repo = here.parents[3]
out = pathlib.Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)
inputs = [repo / "tools/deploy/mysql_test/test_suite/file_sql/data/simple.parquet.b64"]
inputs.extend(sorted((here / "data").glob("*.parquet.b64")))
for source in inputs:
    (out / source.stem).write_bytes(base64.b64decode(source.read_bytes()))
