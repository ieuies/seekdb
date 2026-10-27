// Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
#pragma once
#include "share/ai_service/ai_column_definition.h"
#include "share/ob_dml_sql_splicer.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "sql/engine/dml/ob_dml_ctx_define.h"
#include "sql/engine/dml/ob_table_modify_op.h"
#include "sql/engine/expr/ob_expr_lob_utils.h"
#include "sql/engine/expr/ob_expr_json_func_helper.h"
#include "sql/engine/expr/ob_expr_uuid.h"
#include "sql/session/ai_column_sql.h"
#include <algorithm>

namespace oceanbase {
namespace sql {
class AiColumnDml {
  using Definition = share::AiColumnDefinition;
  using Column = share::schema::ObColumnSchemaV2;
  using Table = share::schema::ObTableSchema;
  static common::ObString str(const std::string &s) {
    return common::ObString(static_cast<int32_t>(s.size()), s.data());
  }
  static std::string quote_name(const common::ObString &name) {
    std::string result = "`";
    for (int32_t i = 0; i < name.length(); ++i) {
      if (name[i] == '`')
        result += '`';
      result += name[i];
    }
    return result + '`';
  }
  static int position(const ObDMLBaseCtDef &ct, uint64_t id) {
    for (int64_t i = 0; i < ct.column_ids_.count(); ++i)
      if (ct.column_ids_.at(i) == id)
        return static_cast<int>(i);
    return -1;
  }
  static int object(ObExpr *expression, ObTableModifyOp &op,
                    common::ObObj &value) {
    int ret = common::OB_SUCCESS;
    common::ObDatum *datum = nullptr;
    if (!expression)
      return common::OB_ERR_UNEXPECTED;
    if (OB_FAIL(expression->eval(op.get_eval_ctx(), datum))) {
    } else if (!datum)
      return common::OB_ERR_UNEXPECTED;
    else
      ret = datum->to_obj(value, expression->obj_meta_);
    return ret;
  }
  static int literal(ObExpr *expression, ObTableModifyOp &op,
                     std::string &result) {
    int ret = common::OB_SUCCESS;
    common::ObObj value;
    common::ObDatum *datum = nullptr;
    OZ(expression->eval(op.get_eval_ctx(), datum));
    if (OB_FAIL(ret))
      return ret;
    if (datum->is_null()) {
      result = "NULL";
      return ret;
    }
    OZ(datum->to_obj(value, expression->obj_meta_));
    if (OB_FAIL(ret))
      return ret;
    if (value.is_string_type() || common::is_lob_storage(value.get_type())) {
      common::ObArenaAllocator allocator;
      common::ObString text;
      OZ(ObTextStringHelper::read_real_string_data(
          op.get_exec_ctx(), allocator, *datum, expression->datum_meta_,
          expression->obj_meta_.has_lob_header(), text));
      if (OB_FAIL(ret))
        return ret;
      if (text.length() > 8 * 1024 * 1024)
        return common::OB_SIZE_OVERFLOW;
      if(value.get_type()==common::ObJsonType){
        common::ObJsonBin json(text.ptr(),text.length(),&allocator);common::ObJsonBuffer buffer(&allocator);
        OZ (json.reset_iter());OZ (json.print(buffer,true,text.length()));
        if(OB_FAIL(ret))return ret;text.assign_ptr(buffer.ptr(),buffer.length());
      }
      const char hex[] = "0123456789abcdef";
      result = value.get_type()==common::ObJsonType?"CAST(CONVERT(X'":"CONVERT(X'";
      for (int32_t i = 0; i < text.length(); ++i) {
        unsigned char ch = text[i];
        result += hex[ch >> 4];
        result += hex[ch & 15];
      }
      result += value.get_type()==common::ObJsonType?"' USING utf8mb4) AS JSON)":"' USING utf8mb4)";
    } else {
      char buffer[1024];
      int64_t length = 0;
      OZ(value.print_plain_str_literal(buffer, sizeof(buffer), length));
      if (OB_SUCC(ret)) {
        result.assign(buffer, static_cast<size_t>(length));
        if (value.get_type() == common::ObDateType ||
            common::ob_is_datetime_tc(value.get_type()))
          result = "'" + result + "'";
      }
    }
    return ret;
  }
  static int set_string(ObExpr *expr, ObTableModifyOp &op,
                        const std::string &value) {
    if (!expr)
      return common::OB_ERR_UNEXPECTED;
    char *buffer = expr->get_str_res_mem(op.get_eval_ctx(), value.size());
    if (!buffer)
      return common::OB_ALLOCATE_MEMORY_FAILED;
    MEMCPY(buffer, value.data(), value.size());
    expr->locate_datum_for_write(op.get_eval_ctx())
        .set_string(buffer, value.size());
    expr->set_evaluated_projected(op.get_eval_ctx());
    return common::OB_SUCCESS;
  }
  static int predicate(const Table &table, const ObDMLBaseCtDef &ct,
                       const ExprFixedArray &row, ObTableModifyOp &op,
                       std::string &result) {
    result.clear();
    for (int64_t i = 0; i < table.get_rowkey_info().get_size(); ++i) {
      const auto *key = table.get_rowkey_info().get_column(i);
      const auto *col = table.get_column_schema(key->column_id_);
      int index = position(ct, key->column_id_);
      if (index < 0 || index >= row.count()) {
        int ret=common::OB_ERR_UNEXPECTED;LOG_WARN("AI row key missing from DML row",K(ret),K(index),K(ct.column_ids_),"row_count",row.count());
        return ret;
      }
      std::string value;
      int ret = literal(row.at(index), op, value);
      if (ret != common::OB_SUCCESS)
        return ret;
      if (!result.empty())
        result += " AND ";
      result += quote_name(col->get_column_name_str()) + " = " + value;
    }
    return result.empty() ? common::OB_ERR_UNEXPECTED : common::OB_SUCCESS;
  }
  static int execute(ObTableModifyOp &op, const common::ObSqlString &sql) {
    int ret = common::OB_SUCCESS;
    AiColumnSql connection;
    int64_t rows = 0;
    OZ(connection.init(op.get_exec_ctx().get_my_session()));
    if (OB_SUCC(ret))
      OZ(connection.get()->execute_write(sql.ptr(), rows));
    if(OB_FAIL(ret))LOG_WARN("AI queue nested SQL failed",K(ret));
    return connection.finish(ret);
  }

public:
  static int process(const ObDMLBaseCtDef &ct, ObTableModifyOp &op,
                     bool inserting, bool deleting = false) {
    using namespace common;
    using namespace share;
    using namespace share::schema;
    int ret = OB_SUCCESS;
    auto *session = op.get_exec_ctx().get_my_session();
    if (!ct.is_primary_index_ || !session)
      return ret;
    auto *guard = op.get_exec_ctx().get_sql_ctx()->schema_guard_;
    const Table *table = nullptr;
    OZ(guard->get_table_schema(ct.das_base_ctdef_.index_tid_, table));
    if (OB_FAIL(ret) || !table)
      return ret;
    std::vector<const Column *> columns;
    for (auto it = table->column_begin(); it != table->column_end(); ++it)
      if ((*it)->is_ai_column())
        columns.push_back(*it);
    if (columns.empty())
      return ret;
    std::string old_key, new_key;
    if (!inserting)
      OZ(predicate(*table, ct, ct.old_row_, op, old_key));
    if (!deleting)
      OZ(predicate(*table, ct, ct.new_row_, op, new_key));
    if (OB_FAIL(ret))
      return ret;
    if (deleting || (!inserting && old_key != new_key)) {
      ObSqlString cleanup;
      OZ(cleanup.append_fmt(
          "DELETE FROM oceanbase.__all_ai_column_task WHERE table_id=%lu AND ",
          table->get_table_id()));
      ObDMLSqlSplicer key;
      OZ(key.add_pk_column("rowkey_predicate",
                           ObHexEscapeSqlStr(str(old_key))));
      ObSqlString condition;
      OZ(key.splice_delete_sql("oceanbase.__all_ai_column_task", condition));
      const char *where = strstr(condition.ptr(), " WHERE ");
      if (where)
        OZ(cleanup.append(where + 7));
      else
        ret = OB_ERR_UNEXPECTED;
      OZ(execute(op, cleanup));
      if (deleting || OB_FAIL(ret))
        return ret;
    }
    const ObDatabaseSchema *database = nullptr;
    OZ(guard->get_database_schema(table->get_database_id(), database));
    if (OB_FAIL(ret) || !database)
      return OB_ERR_UNEXPECTED;
    std::string qualified = quote_name(database->get_database_name_str()) +
                            "." + quote_name(table->get_table_name_str());
    char uuid[UuidCommon::LENGTH_UUID + 1] = {0};
    OZ(ObExprUuid::gen_server_uuid(uuid, UuidCommon::LENGTH_UUID));
    std::string request(uuid, UuidCommon::LENGTH_UUID);
    for (const Column *column : columns) {
      Definition def;
      const auto &encoded = column->get_ai_column_info();
      if (!Definition::decode(std::string(encoded.ptr(), encoded.length()),
                              def))
        return OB_INVALID_ARGUMENT;
      bool changed = inserting || old_key != new_key;
      if (!changed) {
        int ri = position(ct, def.request_column_id);
        ObObj requested;
        if (ri >= 0) {
          OZ(object(ct.new_row_.at(ri), op, requested));
          if (OB_SUCC(ret) && requested.is_string_type() &&
              requested.get_string() == ObString("__ai_retry__"))
            changed = true;
        }
      }
      if (!changed) {
        for (uint64_t dependency : def.dependency_ids) {
          int i = position(ct, dependency);
          if (i < 0 || i >= ct.new_row_.count() || i >= ct.old_row_.count()) {
            ret=OB_ERR_UNEXPECTED;LOG_WARN("AI dependency missing from DML row",K(ret),K(i),K(ct.column_ids_));return ret;
          }
          ObObj old_value, new_value;
          OZ(object(ct.old_row_.at(i), op, old_value));
          OZ(object(ct.new_row_.at(i), op, new_value));
          if (OB_FAIL(ret))
            return ret;
          if (old_value != new_value) {
            changed = true;
            break;
          }
        }
      }
      if (!changed)
        continue;
      int result = position(ct, column->get_column_id()),
          state = position(ct, def.state_column_id),
          request_index = position(ct, def.request_column_id),
          spec = position(ct, def.result_spec_column_id);
      if (result < 0 || state < 0 || request_index < 0 || spec < 0) {
        ret=OB_ERR_UNEXPECTED;LOG_WARN("AI hidden columns missing from DML row",K(ret),K(result),K(state),K(request_index),K(spec),K(ct.column_ids_),"new_row_count",ct.new_row_.count());
        return ret;
      }
      ct.new_row_.at(result)
          ->locate_datum_for_write(op.get_eval_ctx())
          .set_null();
      ct.new_row_.at(result)->set_evaluated_projected(op.get_eval_ctx());
      OZ(set_string(ct.new_row_.at(state), op, "PENDING"));
      OZ(set_string(ct.new_row_.at(request_index), op, request));
      const std::string version = std::to_string(def.definition_version);
      OZ(set_string(ct.new_row_.at(spec), op, version));
      std::string snapshot =
          "SELECT " + def.normalized_expression + " AS ai_result";
      if (!def.dependency_ids.empty()) {
        snapshot += " FROM (SELECT ";
        for (size_t j = 0; j < def.dependency_ids.size(); ++j) {
          int index = position(ct, def.dependency_ids[j]);
          if (index < 0) {ret=OB_ERR_UNEXPECTED;LOG_WARN("AI snapshot dependency missing",K(ret),K(ct.column_ids_));return ret;}
          std::string value;
          OZ(literal(ct.new_row_.at(index), op, value));
          if (OB_FAIL(ret))
            return ret;
          if (j)
            snapshot += ",";
          snapshot += value + " AS " +
                      quote_name(table->get_column_schema(def.dependency_ids[j])
                                     ->get_column_name_str());
        }
        snapshot += ") AS ai_input";
      }
      const auto *state_col = table->get_column_schema(def.state_column_id),
                 *request_col = table->get_column_schema(def.request_column_id),
                 *spec_col =
                     table->get_column_schema(def.result_spec_column_id);
      std::string result_spec =
          column->is_collection()
              ? "VECTOR(" + std::to_string(def.vector_dimension) + ")"
          : column->is_json()                           ? "JSON"
          : ob_is_integer_type(column->get_data_type()) ? "SIGNED"
          : ob_is_numeric_type(column->get_data_type()) ? "DOUBLE"
                                                        : "CHAR";
      ObDMLSqlSplicer dml;
      ObSqlString sql;
      OZ(dml.add_pk_column("request_id", ObHexEscapeSqlStr(str(request))));
      OZ(dml.add_pk_column("column_id", column->get_column_id()));
      OZ(dml.add_column("table_id", table->get_table_id()));
      OZ(dml.add_column("definition_version", def.definition_version));
      OZ(dml.add_column("rowkey_predicate", ObHexEscapeSqlStr(str(new_key))));
      OZ(dml.add_column("qualified_table_name",
                        ObHexEscapeSqlStr(str(qualified))));
      OZ(dml.add_column("normalized_expr", ObHexEscapeSqlStr(str(snapshot))));
      OZ(dml.add_column("result_column_name",
                        ObHexEscapeSqlStr(column->get_column_name_str())));
      OZ(dml.add_column("state_column_name",
                        ObHexEscapeSqlStr(state_col->get_column_name_str())));
      OZ(dml.add_column("request_column_name",
                        ObHexEscapeSqlStr(request_col->get_column_name_str())));
      OZ(dml.add_column("result_spec_column_name",
                        ObHexEscapeSqlStr(spec_col->get_column_name_str())));
      OZ(dml.add_column("result_spec", ObHexEscapeSqlStr(str(result_spec))));
      OZ(dml.add_column("state", 0));
      OZ(dml.add_column("retry_count", 0));
      OZ(dml.add_column("max_retries", def.max_retries));
      OZ(dml.add_time_column("eligible_ts", ObTimeUtility::current_time()));
      OZ(dml.add_gmt_create());
      OZ(dml.add_gmt_modified());
      OZ(dml.splice_insert_sql("oceanbase.__all_ai_column_task", sql));
      OZ(execute(op, sql));
      if (OB_FAIL(ret))
        return ret;
    }
    return ret;
  }

};
} // namespace sql
} // namespace oceanbase
