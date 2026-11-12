#pragma once

#include "duckdb/common/common.hpp"

namespace duckdb {

class BaseFileReader;
class DataChunk;
struct HiveCSVOptions;
struct MultiFileReaderData;

//! With line_fields read_csv returns each line split at its NUL bytes (the delimiter) into the file's columns: read
//! all of them
void ReadOpenCSVLines(BaseFileReader &reader);
//! Replace the columns of a line_fields file the reader's expressions read with the fields SplitOpenCSVLine splits from
//! each line, joined again at its NUL bytes, splitting each line once
void SplitOpenCSVLines(BaseFileReader &reader, const MultiFileReaderData &reader_data, DataChunk &chunk,
                       const HiveCSVOptions &options);

} // namespace duckdb
