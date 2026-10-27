The nullable Parquet fixtures were written with Apache Arrow / PyArrow 26.0.0,
Snappy compression, dictionary encoding, row_group_size=2, and data-page versions
1.0 and 2.0 respectively. Each contains five rows over three row groups, including
an all-NULL BIGINT column, nullable Boolean RLE, UTF-8 text, doubles, DATE and
microsecond timestamps. Expected values are asserted in test_file_scan_utils.cpp.
The script prepare_file_scan_fixtures.py uses only the Python standard library;
PyArrow is not needed to run these tests.
