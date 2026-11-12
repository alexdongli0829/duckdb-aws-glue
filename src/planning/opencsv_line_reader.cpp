#include "planning/opencsv_line_reader.hpp"

#include "core/readers/hive_csv_options.hpp"
#include "core/readers/opencsv_line.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/multi_file/base_file_reader.hpp"
#include "duckdb/common/multi_file/multi_file_states.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"

namespace duckdb {

void ReadOpenCSVLines(BaseFileReader &reader) {
	auto file_columns = reader.GetColumns().size();
	vector<bool> read(file_columns, false);
	for (idx_t i = 0; i < reader.column_ids.size(); i++) {
		auto file_column = reader.column_ids[MultiFileLocalIndex(i)].GetId();
		if (file_column < file_columns) {
			read[file_column] = true;
		}
	}
	for (idx_t file_column = 0; file_column < file_columns; file_column++) {
		if (!read[file_column]) {
			reader.column_ids.push_back(MultiFileLocalColumnId(file_column));
			reader.column_indexes.push_back(ColumnIndex(file_column));
		}
	}
}

void SplitOpenCSVLines(BaseFileReader &reader, const MultiFileReaderData &reader_data, DataChunk &chunk,
                       const HiveCSVOptions &options) {
	vector<bool> used(reader.column_ids.size(), false);
	for (auto &expr : reader_data.expressions) {
		if (!expr) {
			continue;
		}
		ExpressionIterator::VisitExpression<BoundReferenceExpression>(*expr, [&](const BoundReferenceExpression &ref) {
			if (ref.Index() < used.size()) {
				used[ref.Index()] = true;
			}
		});
	}
	auto file_columns = reader.GetColumns().size();
	vector<optional_idx> segments(file_columns);
	vector<pair<idx_t, idx_t>> targets;
	idx_t field_count = 0;
	for (idx_t i = 0; i < reader.column_ids.size(); i++) {
		auto file_column = reader.column_ids[MultiFileLocalIndex(i)].GetId();
		if (file_column >= file_columns) {
			continue;
		}
		if (!segments[file_column].IsValid()) {
			segments[file_column] = i;
		}
		if (used[i]) {
			targets.emplace_back(i, file_column);
			field_count = MaxValue<idx_t>(field_count, file_column + 1);
		}
	}
	for (auto &segment : segments) {
		if (!segment.IsValid()) {
			throw InternalException("Hive text table: a column of the lines of a line_fields file was not read");
		}
	}
	if (targets.empty()) {
		return;
	}
	auto count = chunk.size();
	OpenCSVDialect dialect {options.delimiter, options.quote[0], options.escape[0]};
	vector<Vector> fields_read;
	for (idx_t t = 0; t < targets.size(); t++) {
		fields_read.emplace_back(LogicalType::VARCHAR, count);
	}
	{
		vector<VectorWriter<string_t>> writers;
		for (auto &vector : fields_read) {
			writers.push_back(FlatVector::Writer<string_t>(vector, count));
		}
		vector<VectorIterator<string_t>> pieces;
		pieces.reserve(segments.size());
		for (auto &segment : segments) {
			pieces.push_back(chunk.data[segment.GetIndex()].Values<string_t>());
		}
		vector<string> fields(field_count);
		vector<bool> valid;
		string joined;
		for (idx_t row = 0; row < count; row++) {
			// read_csv splits a line at each NUL byte, the delimiter it is given, and pads the columns after its last
			// piece with NULL
			auto first = pieces[0][row];
			const char *line = nullptr;
			idx_t length = 0;
			if (first.IsValid()) {
				line = first.GetValue().GetData();
				length = first.GetValue().GetSize();
			}
			if (pieces.size() > 1 && pieces[1][row].IsValid()) {
				joined.assign(line, length);
				for (idx_t p = 1; p < pieces.size() && pieces[p][row].IsValid(); p++) {
					auto &piece = pieces[p][row].GetValue();
					joined += '\0';
					joined.append(piece.GetData(), piece.GetSize());
				}
				line = joined.data();
				length = joined.size();
			}
			if (first.IsValid()) {
				SplitOpenCSVLine(line, length, dialect, fields, valid);
			} else {
				valid.assign(field_count, false);
			}
			for (idx_t t = 0; t < targets.size(); t++) {
				auto file_column = targets[t].second;
				if (valid[file_column]) {
					auto &field = fields[file_column];
					writers[t].WriteValue(string_t(field.data(), UnsafeNumericCast<uint32_t>(field.size())));
				} else {
					writers[t].WriteNull();
				}
			}
		}
	}
	for (idx_t t = 0; t < targets.size(); t++) {
		chunk.data[targets[t].first].Reference(fields_read[t]);
	}
}

} // namespace duckdb
