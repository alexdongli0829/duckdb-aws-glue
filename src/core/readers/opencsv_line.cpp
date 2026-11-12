#include "core/readers/opencsv_line.hpp"

#include "duckdb/common/exception.hpp"

namespace duckdb {

namespace {

//! Java's Character.isWhitespace: the Unicode space, line and paragraph separators but the non-breaking spaces
//! (U+00A0, U+2007, U+202F), and the ASCII controls U+0009 to U+000D and U+001C to U+001F
bool IsWhitespace(uint32_t code_point) {
	switch (code_point) {
	case 0x20:
	case 0x1680:
	case 0x2028:
	case 0x2029:
	case 0x205F:
	case 0x3000:
		return true;
	default:
		return (code_point >= 0x09 && code_point <= 0x0D) || (code_point >= 0x1C && code_point <= 0x1F) ||
		       (code_point >= 0x2000 && code_point <= 0x200A && code_point != 0x2007);
	}
}

//! Whether every character of 'text' (UTF-8) is whitespace to Java; a malformed sequence is not
bool IsAllWhitespace(const string &text) {
	idx_t position = 0;
	while (position < text.size()) {
		auto byte = static_cast<uint8_t>(text[position]);
		idx_t size = byte < 0x80 ? 1 : byte >= 0xF0 ? 4 : byte >= 0xE0 ? 3 : byte >= 0xC0 ? 2 : 0;
		if (size == 0 || position + size > text.size()) {
			return false;
		}
		uint32_t code_point = size == 1 ? byte : byte & (0x7F >> size);
		for (idx_t i = 1; i < size; i++) {
			code_point = (code_point << 6) | (static_cast<uint8_t>(text[position + i]) & 0x3F);
		}
		if (!IsWhitespace(code_point)) {
			return false;
		}
		position += size;
	}
	return true;
}

bool IsSeparatorAt(const char *line, idx_t length, idx_t position, const string &separator) {
	return position + separator.size() <= length && memcmp(line + position, separator.data(), separator.size()) == 0;
}

bool IsSeparatorBefore(const char *line, idx_t position, const string &separator) {
	return position >= separator.size() &&
	       memcmp(line + position - separator.size(), separator.data(), separator.size()) == 0;
}

//! As CsvDeserializer.parseLine: hands each of the first field_count fields to 'emit' (index, text), which returns
//! false to stop
template <class EMIT>
void ParseOpenCSVLine(const char *line, idx_t length, const OpenCSVDialect &dialect, idx_t field_count, string &field,
                      EMIT &&emit) {
	if (length == 0 || field_count == 0) {
		return;
	}
	field.clear();
	idx_t field_index = 0;
	bool in_quotes = false;
	bool in_field = false;
	// a position in UTF-16 code units, as Java counts the characters the embedded quote rule looks at
	idx_t char_position = 0;
	for (idx_t position = 0; position < length; position++) {
		auto start = position;
		auto c = line[position];
		auto has_next = position + 1 < length;
		if (c == dialect.escape) {
			if ((in_quotes || in_field) && has_next &&
			    (line[position + 1] == dialect.quote || line[position + 1] == dialect.escape)) {
				field += line[position + 1];
				position++;
			}
		} else if (c == dialect.quote) {
			if ((in_quotes || in_field) && has_next && line[position + 1] == dialect.quote) {
				field += line[position + 1];
				position++;
			} else {
				// a quote in the middle of a field, not next to a separator, is a literal character
				if (char_position > 2 && !IsSeparatorBefore(line, position, dialect.separator) && has_next &&
				    !IsSeparatorAt(line, length, position + 1, dialect.separator)) {
					if (!field.empty() && IsAllWhitespace(field)) {
						field.clear();
					} else {
						field += c;
					}
				}
				in_quotes = !in_quotes;
			}
			in_field = !in_field;
		} else if (!in_quotes && IsSeparatorAt(line, length, position, dialect.separator)) {
			position += dialect.separator.size() - 1;
			if (!emit(field_index, field)) {
				return;
			}
			field_index++;
			if (field_index == field_count) {
				return;
			}
			field.clear();
			in_field = false;
		} else {
			field += c;
			in_field = true;
		}
		for (auto consumed = start; consumed <= position; consumed++) {
			auto byte = static_cast<uint8_t>(line[consumed]);
			if ((byte & 0xC0) != 0x80) {
				// a character above the BMP is two UTF-16 code units
				char_position += byte >= 0xF0 ? 2 : 1;
			}
		}
	}
	if (!in_quotes) {
		emit(field_index, field);
	}
}

} // namespace

void SplitOpenCSVLine(const char *line, idx_t length, const OpenCSVDialect &dialect, vector<string> &fields,
                      vector<bool> &valid) {
	valid.assign(fields.size(), false);
	string field;
	ParseOpenCSVLine(line, length, dialect, fields.size(), field, [&](idx_t index, const string &text) {
		fields[index].assign(text);
		valid[index] = true;
		return true;
	});
}

bool GetOpenCSVLineField(const char *line, idx_t length, const OpenCSVDialect &dialect, idx_t index, string &field) {
	bool found = false;
	string text;
	ParseOpenCSVLine(line, length, dialect, index + 1, text, [&](idx_t field_index, const string &value) {
		if (field_index < index) {
			return true;
		}
		field.assign(value);
		found = true;
		return false;
	});
	return found;
}

} // namespace duckdb
