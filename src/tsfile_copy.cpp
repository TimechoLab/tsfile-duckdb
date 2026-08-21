/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "tsfile_copy.hpp"

#include "duckdb/common/bind_helpers.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/function/copy_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include "cwrapper/errno_define_c.h"
#define ArrowArray  TsFileArrowArray
#define ArrowSchema TsFileArrowSchema
#include "cwrapper/tsfile_cwrapper.h"
#undef ArrowSchema
#undef ArrowArray

#include <limits>

namespace duckdb {

namespace {

struct TsFileCopyBindData final : public FunctionData {
	string table_name;
	string time_column;
	idx_t time_column_index = DConstants::INVALID_INDEX;
	vector<string> column_names;
	vector<LogicalType> column_types;
	vector<TSDataType> ts_types;
	vector<bool> is_tag;

	unique_ptr<FunctionData> Copy() const override {
		auto result = make_uniq<TsFileCopyBindData>();
		result->table_name = table_name;
		result->time_column = time_column;
		result->time_column_index = time_column_index;
		result->column_names = column_names;
		result->column_types = column_types;
		result->ts_types = ts_types;
		result->is_tag = is_tag;
		return std::move(result);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<TsFileCopyBindData>();
		return table_name == other.table_name && time_column == other.time_column &&
		       time_column_index == other.time_column_index && column_names == other.column_names &&
		       column_types == other.column_types && ts_types == other.ts_types && is_tag == other.is_tag;
	}
};

struct TsFileCopyGlobalState final : public GlobalFunctionData {
	WriteFile file = nullptr;
	TsFileWriter writer = nullptr;

	ERRNO Close() {
		if (writer) {
			auto ret = tsfile_writer_close(writer);
			if (ret != RET_OK) {
				return ret;
			}
			writer = nullptr;
		}
		if (file) {
			free_write_file(&file);
		}
		return RET_OK;
	}

	~TsFileCopyGlobalState() override {
		// The COPY framework owns the global state even when a query fails.
		// Retry the close here so that a failed sink does not leave the file
		// handle open. Errors have already been reported by the callback.
		Close();
	}
};

struct TsFileCopyLocalState final : public LocalFunctionData {};

static string OptionString(const string &name, const vector<Value> &values, bool required) {
	if (values.size() != 1 || values[0].IsNull()) {
		if (required) {
			throw BinderException("COPY FORMAT tsfile option %s expects one value", name);
		}
		return string();
	}
	if (values[0].type().id() != LogicalTypeId::VARCHAR) {
		throw BinderException("COPY FORMAT tsfile option %s expects a string value", name);
	}
	return values[0].GetValue<string>();
}

static TSDataType ToTsFileType(const LogicalType &type, const string &column_name) {
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
		return TS_DATATYPE_BOOLEAN;
	case LogicalTypeId::INTEGER:
		return TS_DATATYPE_INT32;
	case LogicalTypeId::BIGINT:
		return TS_DATATYPE_INT64;
	case LogicalTypeId::FLOAT:
		return TS_DATATYPE_FLOAT;
	case LogicalTypeId::DOUBLE:
		return TS_DATATYPE_DOUBLE;
	case LogicalTypeId::VARCHAR:
		return TS_DATATYPE_STRING;
	case LogicalTypeId::BLOB:
		return TS_DATATYPE_BLOB;
	case LogicalTypeId::DATE:
		return TS_DATATYPE_DATE;
	case LogicalTypeId::TIMESTAMP_NS:
		return TS_DATATYPE_TIMESTAMP;
	default:
		throw BinderException("TsFile column '%s' has unsupported DuckDB type %s", column_name, type.ToString());
	}
}

static void ListTsFileCopyOptions(ClientContext &, CopyOptionsInput &input) {
	auto &options = input.options;
	options["table_name"] = CopyOption(LogicalType::VARCHAR, CopyOptionMode::WRITE_ONLY);
	options["time_column"] = CopyOption(LogicalType::VARCHAR, CopyOptionMode::WRITE_ONLY);
	// Column lists are bound as a vector of Values. ANY is the same contract
	// used by DuckDB's built-in PARTITION_BY/FORCE_QUOTE options.
	options["tag_columns"] = CopyOption(LogicalType::ANY, CopyOptionMode::WRITE_ONLY);
}

static unique_ptr<FunctionData> BindTsFileCopy(ClientContext &, CopyFunctionBindInput &input,
                                               const vector<string> &names, const vector<LogicalType> &sql_types) {
	D_ASSERT(names.size() == sql_types.size());
	if (names.empty()) {
		throw BinderException("COPY FORMAT tsfile requires at least one input column");
	}

	auto result = make_uniq<TsFileCopyBindData>();
	result->table_name = "default_table";
	result->time_column = "time";

	vector<Value> tag_values;
	for (auto &option : input.info.options) {
		auto key = StringUtil::Lower(option.first);
		if (key == "table_name") {
			result->table_name = OptionString("TABLE_NAME", option.second, true);
		} else if (key == "time_column") {
			result->time_column = OptionString("TIME_COLUMN", option.second, true);
		} else if (key == "tag_columns") {
			auto converted = ConvertVectorToValue(option.second);
			if (converted.type().id() != LogicalTypeId::LIST || converted.IsNull()) {
				throw BinderException("TAG_COLUMNS expects a column list");
			}
			tag_values = ListValue::GetChildren(converted);
		}
	}
	if (result->table_name.empty()) {
		throw BinderException("TABLE_NAME cannot be empty");
	}
	if (result->time_column.empty()) {
		throw BinderException("TIME_COLUMN cannot be empty");
	}

	result->column_names = names;
	result->column_types = sql_types;
	result->is_tag.resize(names.size(), false);
	result->ts_types.resize(names.size(), TS_DATATYPE_INVALID);

	case_insensitive_map_t<idx_t> name_to_index;
	for (idx_t i = 0; i < names.size(); i++) {
		if (name_to_index.find(names[i]) != name_to_index.end()) {
			throw BinderException("TsFile COPY cannot write duplicate column '%s'", names[i]);
		}
		name_to_index[names[i]] = i;
		if (StringUtil::CIEquals(names[i], result->time_column)) {
			if (result->time_column_index != DConstants::INVALID_INDEX) {
				throw BinderException("TIME_COLUMN '%s' matched more than one column", result->time_column);
			}
			result->time_column_index = i;
		}
	}
	if (result->time_column_index == DConstants::INVALID_INDEX) {
		throw BinderException("TIME_COLUMN '%s' was not found in the input", result->time_column);
	}
	if (result->column_types[result->time_column_index].id() != LogicalTypeId::BIGINT) {
		throw BinderException("TsFile TIME_COLUMN '%s' must have type BIGINT in the first version",
		                      result->column_names[result->time_column_index]);
	}

	if (!tag_values.empty()) {
		case_insensitive_map_t<bool> seen_tags;
		for (auto &tag_value : tag_values) {
			if (tag_value.type().id() != LogicalTypeId::VARCHAR) {
				throw BinderException("TAG_COLUMNS expects column names");
			}
			auto tag_name = tag_value.GetValue<string>();
			auto entry = name_to_index.find(tag_name);
			if (entry == name_to_index.end()) {
				throw BinderException("TAG_COLUMNS expected to find '%s' in the input", tag_name);
			}
			auto tag_index = entry->second;
			if (tag_index == result->time_column_index) {
				throw BinderException("TIME_COLUMN cannot also be a TAG column");
			}
			if (seen_tags.find(tag_name) != seen_tags.end()) {
				throw BinderException("TAG_COLUMNS contains duplicate column '%s'", tag_name);
			}
			seen_tags[tag_name] = true;
			if (result->column_types[tag_index].id() != LogicalTypeId::VARCHAR) {
				throw BinderException("TsFile TAG column '%s' must have type VARCHAR", result->column_names[tag_index]);
			}
			result->is_tag[tag_index] = true;
		}
	}

	idx_t schema_columns = 0;
	for (idx_t i = 0; i < names.size(); i++) {
		if (i == result->time_column_index) {
			continue;
		}
		result->ts_types[i] = ToTsFileType(sql_types[i], names[i]);
		schema_columns++;
	}
	if (schema_columns == 0) {
		throw BinderException("TsFile COPY requires at least one TAG or FIELD column besides TIME_COLUMN");
	}
	return std::move(result);
}

static unique_ptr<LocalFunctionData> InitializeTsFileCopyLocal(ExecutionContext &, FunctionData &) {
	return make_uniq<TsFileCopyLocalState>();
}

static void ThrowTsFileError(const string &operation, ERRNO error) {
	if (error != RET_OK) {
		throw IOException("TsFile %s failed (error %d)", operation, error);
	}
}

static unique_ptr<GlobalFunctionData> InitializeTsFileCopyGlobal(ClientContext &, FunctionData &bind_data,
                                                                 const string &file_path) {
	auto &bind = bind_data.Cast<TsFileCopyBindData>();
	auto result = make_uniq<TsFileCopyGlobalState>();

	ERRNO error = RET_OK;
	result->file = write_file_new(file_path.c_str(), &error);
	if (!result->file || error != RET_OK) {
		if (result->file) {
			free_write_file(&result->file);
		}
		throw IOException("Could not create TsFile '%s' (error %d)", file_path, error);
	}

	vector<ColumnSchema> schemas;
	vector<char *> column_names;
	schemas.reserve(bind.column_names.size() - 1);
	column_names.reserve(bind.column_names.size() - 1);
	for (idx_t i = 0; i < bind.column_names.size(); i++) {
		if (i == bind.time_column_index) {
			continue;
		}
		column_names.emplace_back(const_cast<char *>(bind.column_names[i].c_str()));
		schemas.push_back({column_names.back(), bind.ts_types[i], bind.is_tag[i] ? TAG : FIELD});
	}

	TableSchema schema;
	schema.table_name = const_cast<char *>(bind.table_name.c_str());
	schema.column_schemas = schemas.data();
	schema.column_num = NumericCast<int>(schemas.size());
	result->writer = tsfile_writer_new(result->file, &schema, &error);
	if (!result->writer || error != RET_OK) {
		result->Close();
		throw IOException("Could not initialize TsFile writer for '%s' (error %d)", file_path, error);
	}
	return std::move(result);
}

static void AddTsFileValue(Tablet tablet, const TsFileCopyBindData &bind, idx_t input_column, idx_t tablet_column,
                           idx_t row, idx_t source_row, const UnifiedVectorFormat &format) {
	if (!format.validity.RowIsValid(source_row)) {
		if (bind.is_tag[input_column]) {
			throw InvalidInputException("TsFile TAG column '%s' cannot contain NULL", bind.column_names[input_column]);
		}
		return;
	}

	auto type = bind.ts_types[input_column];
	switch (type) {
	case TS_DATATYPE_BOOLEAN: {
		auto values = UnifiedVectorFormat::GetData<bool>(format);
		ThrowTsFileError("value write",
		                 tablet_add_value_by_index_bool(tablet, NumericCast<uint32_t>(row),
		                                                NumericCast<uint32_t>(tablet_column), values[source_row]));
		break;
	}
	case TS_DATATYPE_INT32: {
		auto values = UnifiedVectorFormat::GetData<int32_t>(format);
		ThrowTsFileError("value write",
		                 tablet_add_value_by_index_int32_t(tablet, NumericCast<uint32_t>(row),
		                                                   NumericCast<uint32_t>(tablet_column), values[source_row]));
		break;
	}
	case TS_DATATYPE_INT64: {
		auto values = UnifiedVectorFormat::GetData<int64_t>(format);
		ThrowTsFileError("value write",
		                 tablet_add_value_by_index_int64_t(tablet, NumericCast<uint32_t>(row),
		                                                   NumericCast<uint32_t>(tablet_column), values[source_row]));
		break;
	}
	case TS_DATATYPE_FLOAT: {
		auto values = UnifiedVectorFormat::GetData<float>(format);
		ThrowTsFileError("value write",
		                 tablet_add_value_by_index_float(tablet, NumericCast<uint32_t>(row),
		                                                 NumericCast<uint32_t>(tablet_column), values[source_row]));
		break;
	}
	case TS_DATATYPE_DOUBLE: {
		auto values = UnifiedVectorFormat::GetData<double>(format);
		ThrowTsFileError("value write",
		                 tablet_add_value_by_index_double(tablet, NumericCast<uint32_t>(row),
		                                                  NumericCast<uint32_t>(tablet_column), values[source_row]));
		break;
	}
	case TS_DATATYPE_DATE: {
		auto values = UnifiedVectorFormat::GetData<date_t>(format);
		int32_t year, month, day;
		Date::Convert(values[source_row], year, month, day);
		const int32_t yyyymmdd = year * 10000 + month * 100 + day;
		ThrowTsFileError("value write",
		                 tablet_add_value_by_index_int32_t(tablet, NumericCast<uint32_t>(row),
		                                                   NumericCast<uint32_t>(tablet_column), yyyymmdd));
		break;
	}
	case TS_DATATYPE_TIMESTAMP: {
		auto values = UnifiedVectorFormat::GetData<timestamp_ns_t>(format);
		ThrowTsFileError("value write", tablet_add_value_by_index_int64_t(tablet, NumericCast<uint32_t>(row),
		                                                                  NumericCast<uint32_t>(tablet_column),
		                                                                  values[source_row].value));
		break;
	}
	case TS_DATATYPE_STRING:
	case TS_DATATYPE_TEXT:
	case TS_DATATYPE_BLOB: {
		auto values = UnifiedVectorFormat::GetData<string_t>(format);
		auto &value = values[source_row];
		ThrowTsFileError("value write", tablet_add_value_by_index_string_with_len(
		                                    tablet, NumericCast<uint32_t>(row), NumericCast<uint32_t>(tablet_column),
		                                    value.GetData(), NumericCast<int>(value.GetSize())));
		break;
	}
	default:
		throw InternalException("Unexpected TsFile type %d", static_cast<int>(type));
	}
}

static void SinkTsFileCopy(ExecutionContext &, FunctionData &bind_data, GlobalFunctionData &global_state,
                           LocalFunctionData &, DataChunk &input) {
	if (input.size() == 0) {
		return;
	}
	auto &bind = bind_data.Cast<TsFileCopyBindData>();
	auto &state = global_state.Cast<TsFileCopyGlobalState>();

	vector<char *> column_names;
	vector<TSDataType> column_types;
	vector<idx_t> tablet_column_for_input(bind.column_names.size(), DConstants::INVALID_INDEX);
	column_names.reserve(bind.column_names.size() - 1);
	column_types.reserve(bind.column_names.size() - 1);
	idx_t tablet_column = 0;
	for (idx_t i = 0; i < bind.column_names.size(); i++) {
		if (i == bind.time_column_index) {
			continue;
		}
		column_names.emplace_back(const_cast<char *>(bind.column_names[i].c_str()));
		column_types.emplace_back(bind.ts_types[i]);
		tablet_column_for_input[i] = tablet_column++;
	}

	auto tablet = tablet_new(column_names.data(), column_types.data(), NumericCast<uint32_t>(column_names.size()),
	                         NumericCast<uint32_t>(input.size()));
	if (!tablet) {
		throw IOException("Could not allocate TsFile tablet");
	}

	try {
		vector<UnifiedVectorFormat> formats(bind.column_names.size());
		for (idx_t i = 0; i < bind.column_names.size(); i++) {
			input.data[i].ToUnifiedFormat(input.size(), formats[i]);
		}

		auto time_values = UnifiedVectorFormat::GetData<int64_t>(formats[bind.time_column_index]);
		for (idx_t row = 0; row < input.size(); row++) {
			auto source_row = formats[bind.time_column_index].sel->get_index(row);
			if (!formats[bind.time_column_index].validity.RowIsValid(source_row)) {
				throw InvalidInputException("TsFile TIME_COLUMN '%s' cannot contain NULL", bind.time_column);
			}
			ThrowTsFileError("timestamp write",
			                 tablet_add_timestamp(tablet, NumericCast<uint32_t>(row), time_values[source_row]));

			for (idx_t i = 0; i < bind.column_names.size(); i++) {
				if (i == bind.time_column_index) {
					continue;
				}
				auto input_row = formats[i].sel->get_index(row);
				AddTsFileValue(tablet, bind, i, tablet_column_for_input[i], row, input_row, formats[i]);
			}
		}

		ThrowTsFileError("tablet write", tsfile_writer_write(state.writer, tablet));
		free_tablet(&tablet);
	} catch (...) {
		if (tablet) {
			free_tablet(&tablet);
		}
		throw;
	}
}

static void CombineTsFileCopy(ExecutionContext &, FunctionData &, GlobalFunctionData &, LocalFunctionData &) {
}

static void FinalizeTsFileCopy(ClientContext &, FunctionData &, GlobalFunctionData &global_state) {
	auto &state = global_state.Cast<TsFileCopyGlobalState>();
	auto error = state.Close();
	ThrowTsFileError("close", error);
}

static CopyFunctionExecutionMode TsFileCopyExecutionMode(bool, bool) {
	return CopyFunctionExecutionMode::REGULAR_COPY_TO_FILE;
}

} // namespace

void RegisterTsFileCopyFunction(ExtensionLoader &loader) {
	CopyFunction function("tsfile");
	function.copy_options = ListTsFileCopyOptions;
	function.copy_to_bind = BindTsFileCopy;
	function.copy_to_initialize_global = InitializeTsFileCopyGlobal;
	function.copy_to_initialize_local = InitializeTsFileCopyLocal;
	function.copy_to_sink = SinkTsFileCopy;
	function.copy_to_combine = CombineTsFileCopy;
	function.copy_to_finalize = FinalizeTsFileCopy;
	function.execution_mode = TsFileCopyExecutionMode;
	function.extension = "tsfile";
	loader.RegisterFunction(function);
}

} // namespace duckdb
