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
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

//===----------------------------------------------------------------------===//
//                         DuckDB
//
// tsfile_extension.cpp
//
//===----------------------------------------------------------------------===//

#include "tsfile_extension.hpp"
#include "tsfile_copy.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/planner/expression/bound_between_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/operator/logical_get.hpp"

#include "cwrapper/errno_define_c.h"
#define ArrowArray  TsFileArrowArray
#define ArrowSchema TsFileArrowSchema
#include "cwrapper/tsfile_cwrapper.h"
#undef ArrowSchema
#undef ArrowArray

#include <cstdlib>
#include <cstring>
#include <limits>

namespace duckdb {

namespace {

struct TsFileSchemaList {
	TableSchema *schemas = nullptr;
	uint32_t count = 0;

	~TsFileSchemaList() {
		if (!schemas) {
			return;
		}
		for (uint32_t i = 0; i < count; i++) {
			free_table_schema(schemas[i]);
		}
		std::free(schemas);
	}
};

enum class TsFileTagFilterKind : uint8_t { COMPARISON, BETWEEN, IS_NULL, IS_NOT_NULL, AND, OR };

struct TsFileTagFilterSpec {
	TsFileTagFilterKind kind = TsFileTagFilterKind::COMPARISON;
	TagFilterOp comparison = TAG_FILTER_EQ;
	string column_name;
	string value;
	string upper;
	string display;
	vector<unique_ptr<TsFileTagFilterSpec>> children;

	unique_ptr<TsFileTagFilterSpec> Copy() const {
		auto result = make_uniq<TsFileTagFilterSpec>();
		result->kind = kind;
		result->comparison = comparison;
		result->column_name = column_name;
		result->value = value;
		result->upper = upper;
		result->display = display;
		for (auto &child : children) {
			result->children.emplace_back(child->Copy());
		}
		return result;
	}

	bool Equals(const TsFileTagFilterSpec &other) const {
		if (kind != other.kind || comparison != other.comparison || column_name != other.column_name ||
		    value != other.value || upper != other.upper || display != other.display ||
		    children.size() != other.children.size()) {
			return false;
		}
		for (idx_t i = 0; i < children.size(); i++) {
			if (!children[i]->Equals(*other.children[i])) {
				return false;
			}
		}
		return true;
	}
};

struct TsFileBindData : public TableFunctionData {
	string file_path;
	string table_name;
	vector<string> column_names;
	vector<TSDataType> column_types;
	vector<ColumnCategory> column_categories;
	vector<unique_ptr<TsFileTagFilterSpec>> tag_filters;
	int64_t start_time = std::numeric_limits<int64_t>::min();
	int64_t end_time = std::numeric_limits<int64_t>::max();
	bool empty = false;

	unique_ptr<FunctionData> Copy() const override {
		auto result = make_uniq<TsFileBindData>();
		result->column_ids = column_ids;
		result->file_path = file_path;
		result->table_name = table_name;
		result->column_names = column_names;
		result->column_types = column_types;
		result->column_categories = column_categories;
		for (auto &tag_filter : tag_filters) {
			result->tag_filters.emplace_back(tag_filter->Copy());
		}
		result->start_time = start_time;
		result->end_time = end_time;
		result->empty = empty;
		return std::move(result);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<TsFileBindData>();
		if (file_path != other.file_path || table_name != other.table_name || column_names != other.column_names ||
		    column_types != other.column_types || column_categories != other.column_categories ||
		    start_time != other.start_time || end_time != other.end_time || empty != other.empty ||
		    tag_filters.size() != other.tag_filters.size()) {
			return false;
		}
		for (idx_t i = 0; i < tag_filters.size(); i++) {
			if (!tag_filters[i]->Equals(*other.tag_filters[i])) {
				return false;
			}
		}
		return true;
	}
};

struct TsFileScanState : public GlobalTableFunctionState {
	TsFileReader reader = nullptr;
	ResultSet result_set = nullptr;
	TagFilterHandle tag_filter = nullptr;
	TsFileArrowArray current_array {};
	TsFileArrowSchema current_schema {};
	vector<idx_t> arrow_child_for_output;
	vector<TSDataType> output_types;
	bool finished = false;

	idx_t MaxThreads() const override {
		return 1;
	}

	void ReleaseArrowBatch() {
		if (current_array.release) {
			current_array.release(&current_array);
		}
		if (current_schema.release) {
			current_schema.release(&current_schema);
		}
	}

	~TsFileScanState() override {
		ReleaseArrowBatch();
		if (result_set) {
			free_tsfile_result_set(&result_set);
		}
		if (tag_filter) {
			tsfile_tag_filter_free(tag_filter);
		}
		if (reader) {
			tsfile_reader_close(reader);
			reader = nullptr;
		}
	}
};

static bool TryGetTsFileColumnIndex(const Expression &expression, const LogicalGet &get, idx_t &column_index) {
	if (expression.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
		return false;
	}
	auto &column = expression.Cast<BoundColumnRefExpression>();
	if (column.depth != 0 || column.binding.table_index != get.table_index ||
	    column.binding.column_index >= get.GetColumnIds().size()) {
		return false;
	}
	auto &projected_column = get.GetColumnIds()[column.binding.column_index];
	if (!projected_column.HasPrimaryIndex() || projected_column.IsVirtualColumn()) {
		return false;
	}
	column_index = projected_column.GetPrimaryIndex();
	return true;
}

static bool IsTimeColumn(const Expression &expression, const LogicalGet &get) {
	idx_t column_index;
	return TryGetTsFileColumnIndex(expression, get, column_index) && column_index == 0;
}

static bool TryGetTimeConstant(const Expression &expression, int64_t &result) {
	if (expression.GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
		return false;
	}
	auto value = expression.Cast<BoundConstantExpression>().value;
	if (value.IsNull() || !value.DefaultTryCastAs(LogicalType::BIGINT)) {
		return false;
	}
	result = BigIntValue::Get(value);
	return true;
}

static bool ApplyTimeComparison(TsFileBindData &bind_data, ExpressionType comparison, int64_t value) {
	switch (comparison) {
	case ExpressionType::COMPARE_EQUAL:
		bind_data.start_time = MaxValue(bind_data.start_time, value);
		bind_data.end_time = MinValue(bind_data.end_time, value);
		break;
	case ExpressionType::COMPARE_GREATERTHAN:
		if (value == std::numeric_limits<int64_t>::max()) {
			bind_data.empty = true;
		} else {
			bind_data.start_time = MaxValue(bind_data.start_time, value + 1);
		}
		break;
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		bind_data.start_time = MaxValue(bind_data.start_time, value);
		break;
	case ExpressionType::COMPARE_LESSTHAN:
		if (value == std::numeric_limits<int64_t>::min()) {
			bind_data.empty = true;
		} else {
			bind_data.end_time = MinValue(bind_data.end_time, value - 1);
		}
		break;
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		bind_data.end_time = MinValue(bind_data.end_time, value);
		break;
	default:
		return false;
	}
	bind_data.empty = bind_data.empty || bind_data.start_time > bind_data.end_time;
	return true;
}

static bool TryPushdownTimeComparison(LogicalGet &get, TsFileBindData &bind_data, Expression &expression) {
	if (expression.GetExpressionClass() == ExpressionClass::BOUND_BETWEEN) {
		auto &between = expression.Cast<BoundBetweenExpression>();
		int64_t lower;
		int64_t upper;
		if (!IsTimeColumn(*between.input, get) || !TryGetTimeConstant(*between.lower, lower) ||
		    !TryGetTimeConstant(*between.upper, upper)) {
			return false;
		}
		ApplyTimeComparison(bind_data, between.LowerComparisonType(), lower);
		ApplyTimeComparison(bind_data, between.UpperComparisonType(), upper);
		return true;
	}

	if (expression.GetExpressionClass() != ExpressionClass::BOUND_COMPARISON) {
		return false;
	}
	auto &comparison = expression.Cast<BoundComparisonExpression>();
	int64_t value;
	if (IsTimeColumn(*comparison.left, get) && TryGetTimeConstant(*comparison.right, value)) {
		return ApplyTimeComparison(bind_data, comparison.GetExpressionType(), value);
	}
	if (IsTimeColumn(*comparison.right, get) && TryGetTimeConstant(*comparison.left, value)) {
		auto comparison_type = comparison.GetExpressionType();
		switch (comparison_type) {
		case ExpressionType::COMPARE_EQUAL:
		case ExpressionType::COMPARE_GREATERTHAN:
		case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		case ExpressionType::COMPARE_LESSTHAN:
		case ExpressionType::COMPARE_LESSTHANOREQUALTO:
			return ApplyTimeComparison(bind_data, FlipComparisonExpression(comparison_type), value);
		default:
			return false;
		}
	}
	return false;
}

static bool TryGetTagColumnIndex(const Expression &expression, const LogicalGet &get, const TsFileBindData &bind_data,
                                 idx_t &column_index) {
	if (!TryGetTsFileColumnIndex(expression, get, column_index) || column_index >= bind_data.column_names.size()) {
		return false;
	}
	return bind_data.column_categories[column_index] == TAG;
}

static bool TryGetStringConstant(const Expression &expression, string &value) {
	if (expression.GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
		return false;
	}
	auto constant = expression.Cast<BoundConstantExpression>().value;
	if (constant.IsNull() || constant.type().id() != LogicalTypeId::VARCHAR) {
		return false;
	}
	value = StringValue::Get(constant);
	return true;
}

static bool TryGetTagFilterOp(ExpressionType comparison, TagFilterOp &op) {
	switch (comparison) {
	case ExpressionType::COMPARE_EQUAL:
		op = TAG_FILTER_EQ;
		return true;
	case ExpressionType::COMPARE_NOTEQUAL:
		op = TAG_FILTER_NEQ;
		return true;
	case ExpressionType::COMPARE_LESSTHAN:
		op = TAG_FILTER_LT;
		return true;
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		op = TAG_FILTER_LTEQ;
		return true;
	case ExpressionType::COMPARE_GREATERTHAN:
		op = TAG_FILTER_GT;
		return true;
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		op = TAG_FILTER_GTEQ;
		return true;
	default:
		return false;
	}
}

static unique_ptr<TsFileTagFilterSpec> TryCreateTagFilterSpec(const Expression &expression, const LogicalGet &get,
                                                              const TsFileBindData &bind_data) {
	if (expression.GetExpressionClass() == ExpressionClass::BOUND_COMPARISON) {
		auto &comparison = expression.Cast<BoundComparisonExpression>();
		auto comparison_type = comparison.GetExpressionType();
		idx_t tag_index;
		string value;
		if (TryGetTagColumnIndex(*comparison.left, get, bind_data, tag_index) &&
		    TryGetStringConstant(*comparison.right, value)) {
			// Already in column OP constant form.
		} else if (TryGetTagColumnIndex(*comparison.right, get, bind_data, tag_index) &&
		           TryGetStringConstant(*comparison.left, value)) {
			comparison_type = FlipComparisonExpression(comparison_type);
		} else {
			return nullptr;
		}
		TagFilterOp op;
		if (!TryGetTagFilterOp(comparison_type, op)) {
			return nullptr;
		}
		auto result = make_uniq<TsFileTagFilterSpec>();
		result->kind = TsFileTagFilterKind::COMPARISON;
		result->comparison = op;
		result->column_name = bind_data.column_names[tag_index];
		result->value = value;
		result->display = expression.ToString();
		return result;
	}

	if (expression.GetExpressionClass() == ExpressionClass::BOUND_BETWEEN) {
		auto &between = expression.Cast<BoundBetweenExpression>();
		idx_t tag_index;
		string lower;
		string upper;
		if (!between.lower_inclusive || !between.upper_inclusive ||
		    !TryGetTagColumnIndex(*between.input, get, bind_data, tag_index) ||
		    !TryGetStringConstant(*between.lower, lower) || !TryGetStringConstant(*between.upper, upper)) {
			return nullptr;
		}
		auto result = make_uniq<TsFileTagFilterSpec>();
		result->kind = TsFileTagFilterKind::BETWEEN;
		result->column_name = bind_data.column_names[tag_index];
		result->value = lower;
		result->upper = upper;
		result->display = expression.ToString();
		return result;
	}

	if (expression.GetExpressionClass() == ExpressionClass::BOUND_OPERATOR) {
		auto &op = expression.Cast<BoundOperatorExpression>();
		if (op.GetExpressionType() == ExpressionType::OPERATOR_IS_NULL ||
		    op.GetExpressionType() == ExpressionType::OPERATOR_IS_NOT_NULL) {
			idx_t tag_index;
			if (op.children.size() != 1 || !TryGetTagColumnIndex(*op.children[0], get, bind_data, tag_index)) {
				return nullptr;
			}
			auto result = make_uniq<TsFileTagFilterSpec>();
			result->kind = op.GetExpressionType() == ExpressionType::OPERATOR_IS_NULL
			                   ? TsFileTagFilterKind::IS_NULL
			                   : TsFileTagFilterKind::IS_NOT_NULL;
			result->column_name = bind_data.column_names[tag_index];
			result->display = expression.ToString();
			return result;
		}
		return nullptr;
	}

	if (expression.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conjunction = expression.Cast<BoundConjunctionExpression>();
		if (conjunction.children.empty() || (conjunction.GetExpressionType() != ExpressionType::CONJUNCTION_AND &&
		                                     conjunction.GetExpressionType() != ExpressionType::CONJUNCTION_OR)) {
			return nullptr;
		}
		auto result = make_uniq<TsFileTagFilterSpec>();
		result->kind = conjunction.GetExpressionType() == ExpressionType::CONJUNCTION_AND ? TsFileTagFilterKind::AND
		                                                                                  : TsFileTagFilterKind::OR;
		result->display = expression.ToString();
		for (auto &child : conjunction.children) {
			auto child_filter = TryCreateTagFilterSpec(*child, get, bind_data);
			if (!child_filter) {
				return nullptr;
			}
			result->children.emplace_back(std::move(child_filter));
		}
		return result;
	}

	return nullptr;
}

static TagFilterHandle BuildTsFileTagFilter(const TsFileTagFilterSpec &filter, const string &table_name,
                                            TsFileReader reader) {
	if (filter.kind == TsFileTagFilterKind::COMPARISON) {
		ERRNO error = RET_OK;
		auto result = tsfile_tag_filter_create(reader, table_name.c_str(), filter.column_name.c_str(),
		                                       filter.value.c_str(), filter.comparison, &error);
		if (error != RET_OK && result) {
			tsfile_tag_filter_free(result);
			return nullptr;
		}
		return result;
	}

	if (filter.kind == TsFileTagFilterKind::BETWEEN) {
		ERRNO error = RET_OK;
		auto result = tsfile_tag_filter_between(reader, table_name.c_str(), filter.column_name.c_str(),
		                                        filter.value.c_str(), filter.upper.c_str(), false, &error);
		if (error != RET_OK && result) {
			tsfile_tag_filter_free(result);
			return nullptr;
		}
		return result;
	}

	if (filter.kind == TsFileTagFilterKind::IS_NULL || filter.kind == TsFileTagFilterKind::IS_NOT_NULL) {
		ERRNO error = RET_OK;
		auto op = filter.kind == TsFileTagFilterKind::IS_NULL ? TAG_FILTER_IS_NULL : TAG_FILTER_IS_NOT_NULL;
		// The current C wrapper validates value even for NULL-aware operators;
		// the value is ignored by the underlying builder.
		auto result = tsfile_tag_filter_create(reader, table_name.c_str(), filter.column_name.c_str(), "", op, &error);
		if (error != RET_OK && result) {
			tsfile_tag_filter_free(result);
			return nullptr;
		}
		return result;
	}

	if (filter.kind == TsFileTagFilterKind::AND || filter.kind == TsFileTagFilterKind::OR) {
		if (filter.children.empty()) {
			return nullptr;
		}
		TagFilterHandle result = nullptr;
		for (auto &child_filter : filter.children) {
			auto child = BuildTsFileTagFilter(*child_filter, table_name, reader);
			if (!child) {
				if (result) {
					tsfile_tag_filter_free(result);
				}
				return nullptr;
			}
			if (!result) {
				result = child;
				continue;
			}
			auto combined = filter.kind == TsFileTagFilterKind::AND ? tsfile_tag_filter_and(result, child)
			                                                        : tsfile_tag_filter_or(result, child);
			if (!combined) {
				tsfile_tag_filter_free(result);
				tsfile_tag_filter_free(child);
				return nullptr;
			}
			result = combined;
		}
		return result;
	}

	return nullptr;
}

static void TsFileComplexFilterPushdown(ClientContext &, LogicalGet &get, FunctionData *bind_data_p,
                                        vector<unique_ptr<Expression>> &filters) {
	auto &bind_data = bind_data_p->Cast<TsFileBindData>();
	for (idx_t filter_index = 0; filter_index < filters.size();) {
		if (TryPushdownTimeComparison(get, bind_data, *filters[filter_index])) {
			filters.erase_at(filter_index);
		} else {
			auto tag_filter = TryCreateTagFilterSpec(*filters[filter_index], get, bind_data);
			if (tag_filter) {
				bind_data.tag_filters.emplace_back(std::move(tag_filter));
				filters.erase_at(filter_index);
			} else {
				filter_index++;
			}
		}
	}
}

static InsertionOrderPreservingMap<string> TsFileToString(TableFunctionToStringInput &input) {
	auto &bind_data = input.bind_data->Cast<TsFileBindData>();
	InsertionOrderPreservingMap<string> result;
	result["File"] = bind_data.file_path;
	result["Table"] = bind_data.table_name;
	if (bind_data.empty) {
		result["Time Range"] = "empty";
	} else if (bind_data.start_time != std::numeric_limits<int64_t>::min() ||
	           bind_data.end_time != std::numeric_limits<int64_t>::max()) {
		result["Time Range"] = StringUtil::Format("[%lld, %lld]", bind_data.start_time, bind_data.end_time);
	}
	if (!bind_data.tag_filters.empty()) {
		string tag_filter_text;
		for (auto &tag_filter : bind_data.tag_filters) {
			if (!tag_filter_text.empty()) {
				tag_filter_text += " AND ";
			}
			tag_filter_text += tag_filter->display;
		}
		result["TAG Filter"] = tag_filter_text;
	}
	return result;
}

static LogicalType ToDuckDBType(TSDataType type) {
	switch (type) {
	case TS_DATATYPE_BOOLEAN:
		return LogicalType::BOOLEAN;
	case TS_DATATYPE_INT32:
		return LogicalType::INTEGER;
	case TS_DATATYPE_INT64:
		return LogicalType::BIGINT;
	case TS_DATATYPE_FLOAT:
		return LogicalType::FLOAT;
	case TS_DATATYPE_DOUBLE:
		return LogicalType::DOUBLE;
	case TS_DATATYPE_TEXT:
	case TS_DATATYPE_STRING:
		return LogicalType::VARCHAR;
	case TS_DATATYPE_TIMESTAMP:
		return LogicalType::TIMESTAMP_NS;
	case TS_DATATYPE_DATE:
		return LogicalType::DATE;
	case TS_DATATYPE_BLOB:
		return LogicalType::BLOB;
	default:
		throw NotImplementedException("Unsupported TsFile data type: %d", static_cast<int>(type));
	}
}

static unique_ptr<FunctionData> TsFileScanBind(ClientContext &, TableFunctionBindInput &input,
                                               vector<LogicalType> &return_types, vector<string> &names) {
	if (input.inputs[0].IsNull() || input.inputs[1].IsNull()) {
		throw BinderException("read_tsfile path and table arguments cannot be NULL");
	}

	auto result = make_uniq<TsFileBindData>();
	result->file_path = StringValue::Get(input.inputs[0]);
	result->table_name = StringUtil::Lower(StringValue::Get(input.inputs[1]));

	ERRNO error = RET_OK;
	TsFileReader reader = tsfile_reader_new(result->file_path.c_str(), &error);
	if (!reader || error != RET_OK) {
		if (reader) {
			tsfile_reader_close(reader);
		}
		throw IOException("Could not open TsFile '%s' (TsFile error %d)", result->file_path, error);
	}

	TsFileSchemaList schema_list;
	schema_list.schemas = tsfile_reader_get_all_table_schemas(reader, &schema_list.count);
	tsfile_reader_close(reader);

	const TableSchema *selected_schema = nullptr;
	for (uint32_t i = 0; i < schema_list.count; i++) {
		if (schema_list.schemas[i].table_name &&
		    StringUtil::Lower(schema_list.schemas[i].table_name) == result->table_name) {
			selected_schema = &schema_list.schemas[i];
			break;
		}
	}
	if (!selected_schema) {
		throw BinderException("Table '%s' was not found in TsFile '%s'", result->table_name, result->file_path);
	}

	// The table query result always prepends the global time axis. It remains a
	// BIGINT because the TsFile format deliberately leaves its unit to the
	// protocol using the file.
	names.emplace_back("time");
	return_types.emplace_back(LogicalType::BIGINT);
	result->column_names.emplace_back("time");
	result->column_types.emplace_back(TS_DATATYPE_INT64);
	result->column_categories.emplace_back(FIELD);

	for (int i = 0; i < selected_schema->column_num; i++) {
		auto &column = selected_schema->column_schemas[i];
		if (!column.column_name) {
			throw InvalidInputException("TsFile table '%s' contains a column without a name", result->table_name);
		}
		names.emplace_back(column.column_name);
		return_types.emplace_back(ToDuckDBType(column.data_type));
		result->column_names.emplace_back(column.column_name);
		result->column_types.emplace_back(column.data_type);
		result->column_categories.emplace_back(column.column_category);
	}

	return std::move(result);
}

static unique_ptr<GlobalTableFunctionState> TsFileScanInit(ClientContext &, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<TsFileBindData>();
	auto state = make_uniq<TsFileScanState>();
	if (bind_data.empty) {
		state->finished = true;
		return std::move(state);
	}

	ERRNO error = RET_OK;
	state->reader = tsfile_reader_new(bind_data.file_path.c_str(), &error);
	if (!state->reader || error != RET_OK) {
		throw IOException("Could not open TsFile '%s' (TsFile error %d)", bind_data.file_path, error);
	}
	for (auto &tag_filter_expression : bind_data.tag_filters) {
		auto tag_filter = BuildTsFileTagFilter(*tag_filter_expression, bind_data.table_name, state->reader);
		if (!tag_filter) {
			throw IOException("Could not construct TAG filter for table '%s' in TsFile '%s'", bind_data.table_name,
			                  bind_data.file_path);
		}
		if (!state->tag_filter) {
			state->tag_filter = tag_filter;
		} else {
			auto combined = tsfile_tag_filter_and(state->tag_filter, tag_filter);
			if (!combined) {
				tsfile_tag_filter_free(state->tag_filter);
				tsfile_tag_filter_free(tag_filter);
				state->tag_filter = nullptr;
				throw IOException("Could not combine TAG filters for table '%s' in TsFile '%s'", bind_data.table_name,
				                  bind_data.file_path);
			}
			state->tag_filter = combined;
		}
	}

	vector<string> query_columns;
	for (auto column_id : input.column_ids) {
		if (column_id == COLUMN_IDENTIFIER_ROW_ID) {
			state->arrow_child_for_output.emplace_back(DConstants::INVALID_INDEX);
			state->output_types.emplace_back(TS_DATATYPE_INVALID);
			continue;
		}
		if (column_id >= bind_data.column_names.size()) {
			throw InternalException("Invalid projected read_tsfile column index");
		}
		state->output_types.emplace_back(bind_data.column_types[column_id]);
		if (column_id == 0) {
			state->arrow_child_for_output.emplace_back(0);
		} else {
			query_columns.emplace_back(bind_data.column_names[column_id]);
			state->arrow_child_for_output.emplace_back(query_columns.size());
		}
	}

	// TsFile needs one physical measurement to drive its time-ordered scan.
	// This also covers SELECT time and SELECT count(*) projections.
	if (query_columns.empty()) {
		if (bind_data.column_names.size() <= 1) {
			throw InvalidInputException("TsFile table '%s' has no measurement columns", bind_data.table_name);
		}
		query_columns.emplace_back(bind_data.column_names[1]);
	}

	vector<char *> query_column_ptrs;
	query_column_ptrs.reserve(query_columns.size());
	for (auto &column : query_columns) {
		query_column_ptrs.emplace_back(const_cast<char *>(column.c_str()));
	}

	state->result_set =
	    tsfile_query_table_batch(state->reader, bind_data.table_name.c_str(), query_column_ptrs.data(),
	                             static_cast<uint32_t>(query_column_ptrs.size()), bind_data.start_time,
	                             bind_data.end_time, state->tag_filter, static_cast<int>(STANDARD_VECTOR_SIZE), &error);
	if (!state->result_set || error != RET_OK) {
		throw IOException("Could not query table '%s' in TsFile '%s' (TsFile error %d)", bind_data.table_name,
		                  bind_data.file_path, error);
	}

	return std::move(state);
}

static bool ArrowValueIsValid(const TsFileArrowArray &array, idx_t row) {
	if (!array.buffers[0]) {
		return true;
	}
	auto validity = static_cast<const uint8_t *>(array.buffers[0]);
	auto source_row = static_cast<idx_t>(array.offset) + row;
	return validity[source_row / 8] & static_cast<uint8_t>(1U << (source_row & 7U));
}

template <class SOURCE, class TARGET = SOURCE>
static void CopyFixedWidth(const TsFileArrowArray &array, Vector &output, idx_t count) {
	auto source = static_cast<const SOURCE *>(array.buffers[1]) + array.offset;
	auto target = FlatVector::GetData<TARGET>(output);
	for (idx_t row = 0; row < count; row++) {
		target[row] = TARGET(source[row]);
	}
}

static void CopyArrowColumn(const TsFileArrowArray &array, TSDataType type, Vector &output, idx_t count) {
	auto &validity = FlatVector::Validity(output);
	validity.SetAllValid(count);
	for (idx_t row = 0; row < count; row++) {
		if (!ArrowValueIsValid(array, row)) {
			validity.SetInvalid(row);
		}
	}

	switch (type) {
	case TS_DATATYPE_BOOLEAN: {
		auto source = static_cast<const uint8_t *>(array.buffers[1]);
		auto target = FlatVector::GetData<bool>(output);
		for (idx_t row = 0; row < count; row++) {
			auto source_row = static_cast<idx_t>(array.offset) + row;
			target[row] = source[source_row / 8] & static_cast<uint8_t>(1U << (source_row & 7U));
		}
		break;
	}
	case TS_DATATYPE_INT32:
		CopyFixedWidth<int32_t>(array, output, count);
		break;
	case TS_DATATYPE_INT64:
		CopyFixedWidth<int64_t>(array, output, count);
		break;
	case TS_DATATYPE_FLOAT:
		CopyFixedWidth<float>(array, output, count);
		break;
	case TS_DATATYPE_DOUBLE:
		CopyFixedWidth<double>(array, output, count);
		break;
	case TS_DATATYPE_TIMESTAMP:
		CopyFixedWidth<int64_t, timestamp_ns_t>(array, output, count);
		break;
	case TS_DATATYPE_DATE:
		CopyFixedWidth<int32_t, date_t>(array, output, count);
		break;
	case TS_DATATYPE_TEXT:
	case TS_DATATYPE_STRING:
	case TS_DATATYPE_BLOB: {
		auto offsets = static_cast<const int32_t *>(array.buffers[1]);
		auto data = static_cast<const char *>(array.buffers[2]);
		auto target = FlatVector::GetData<string_t>(output);
		for (idx_t row = 0; row < count; row++) {
			if (!ArrowValueIsValid(array, row)) {
				continue;
			}
			auto source_row = static_cast<idx_t>(array.offset) + row;
			auto begin = offsets[source_row];
			auto length = offsets[source_row + 1] - begin;
			target[row] = StringVector::AddStringOrBlob(output, data + begin, NumericCast<idx_t>(length));
		}
		break;
	}
	default:
		throw InternalException("Unexpected TsFile output type: %d", static_cast<int>(type));
	}
}

static void TsFileScanFunction(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<TsFileScanState>();
	if (state.finished) {
		return;
	}
	state.ReleaseArrowBatch();

	ERRNO error =
	    tsfile_result_set_get_next_tsblock_as_arrow(state.result_set, &state.current_array, &state.current_schema);
	if (error == RET_NO_MORE_DATA) {
		state.finished = true;
		return;
	}
	if (error != RET_OK) {
		throw IOException("TsFile batch scan failed (TsFile error %d)", error);
	}

	auto count = NumericCast<idx_t>(state.current_array.length);
	if (count > STANDARD_VECTOR_SIZE) {
		throw InternalException("TsFile returned a batch larger than DuckDB's standard vector size");
	}
	if (state.arrow_child_for_output.size() != output.ColumnCount()) {
		throw InternalException("read_tsfile projection mapping does not match the output chunk");
	}

	for (idx_t output_column = 0; output_column < state.arrow_child_for_output.size(); output_column++) {
		auto arrow_child = state.arrow_child_for_output[output_column];
		if (arrow_child == DConstants::INVALID_INDEX) {
			continue;
		}
		if (arrow_child >= static_cast<idx_t>(state.current_array.n_children)) {
			throw InternalException("TsFile Arrow batch is missing a projected column");
		}
		CopyArrowColumn(*state.current_array.children[arrow_child], state.output_types[output_column],
		                output.data[output_column], count);
	}
	output.SetCardinality(count);
}

static void LoadInternal(ExtensionLoader &loader) {
	TableFunction read_tsfile("read_tsfile", {LogicalType::VARCHAR, LogicalType::VARCHAR}, TsFileScanFunction,
	                          TsFileScanBind, TsFileScanInit);
	read_tsfile.projection_pushdown = true;
	read_tsfile.pushdown_complex_filter = TsFileComplexFilterPushdown;
	read_tsfile.to_string = TsFileToString;
	loader.RegisterFunction(read_tsfile);
	RegisterTsFileCopyFunction(loader);
}

} // namespace

void TsfileExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string TsfileExtension::Name() {
	return "tsfile";
}

std::string TsfileExtension::Version() const {
#ifdef EXT_VERSION_TSFILE
	return EXT_VERSION_TSFILE;
#else
	return "0.1.0";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(tsfile, loader) { // NOLINT
	duckdb::LoadInternal(loader);
}
}
