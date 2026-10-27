/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX SQL_ENG

#include "sql/engine/basic/ob_function_table_op.h"
#include "sql/engine/basic/file_scan_utils.h"
#include <memory>
#include "sql/engine/ob_exec_context.h"
#include "sql/engine/expr/ob_expr_lob_utils.h"


namespace oceanbase
{
using namespace common;
namespace sql
{

OB_SERIALIZE_MEMBER((ObFunctionTableSpec, ObOpSpec), value_expr_, column_exprs_, has_correlated_expr_, file_scan_, file_column_ids_);


namespace {
struct FileScanState {
  file_sql::Bound bound;
  std::unique_ptr<file_sql::Reader> reader;
  std::vector<file_sql::Cell> cells;
  std::vector<std::string> files;
  std::vector<file_sql::Column> description;
  size_t position = 0;
};
}
void ObFunctionTableOp::close_file_scan()
{
  if (file_state_ != nullptr) {
    static_cast<FileScanState *>(file_state_)->~FileScanState();
    ctx_.get_allocator().free(file_state_);
    file_state_ = nullptr;
  }
}
int ObFunctionTableOp::open_file_scan()
{
  int ret = OB_SUCCESS;
  ObObj enabled;
  ObString secure;
  ObDatum *payload = nullptr;
  auto *session = ctx_.get_my_session();
  if (session == nullptr) return OB_ERR_UNEXPECTED;
  OZ (session->get_sys_variable(share::SYS_VAR_ENABLE_FILE_SQL,enabled));
  if (OB_SUCC(ret) && enabled.get_int() == 0) {
    ret = OB_NOT_SUPPORTED; LOG_USER_ERROR(OB_NOT_SUPPORTED,"file SQL is disabled");
  }
  OZ (session->get_secure_file_priv(secure));
  OZ (MY_SPEC.value_expr_->eval(eval_ctx_,payload));
  if (OB_FAIL(ret)) return ret;
  if (payload == nullptr || payload->is_null()) return OB_ERR_UNEXPECTED;
  void *memory = file_state_;
  if (memory != nullptr) static_cast<FileScanState *>(memory)->~FileScanState();
  else memory = ctx_.get_allocator().alloc(sizeof(FileScanState));
  if (memory == nullptr) return OB_ALLOCATE_MEMORY_FAILED;
  auto *state = new (memory) FileScanState();
  file_state_ = state;
  ObString encoded = payload->get_string();
  if (!file_sql::unpack(std::string(encoded.ptr(),encoded.length()),state->bound)) return OB_ERR_UNEXPECTED;
  std::string root(secure.ptr(),secure.length()),path,error;
  if (state->bound.discover) {
    if (!file_sql::discover(state->bound.options,root,state->files,error)) ret = OB_ERR_NO_PRIV_DIRECT_PATH_ACCESS;
  } else if (!file_sql::secure_path(state->bound.options.path,root,path,error)) {
    ret = OB_ERR_NO_PRIV_DIRECT_PATH_ACCESS;
  } else {
    state->bound.options.path = path;
    state->reader.reset(new (std::nothrow) file_sql::Reader(state->bound.options));
    if (!state->reader) ret = OB_ALLOCATE_MEMORY_FAILED;
    else {
      state->reader->set_schema(state->bound.columns);
      std::vector<size_t> projection;
      for(int64_t i=0;i<MY_SPEC.file_column_ids_.count();++i) projection.push_back(MY_SPEC.file_column_ids_.at(i));
      state->reader->projection(projection);
      if (!state->reader->open(&state->bound.stamp)) { error = state->reader->error(); ret = OB_INVALID_ARGUMENT; }
    }
  }
  if (OB_SUCC(ret) && state->bound.describe) {
    state->description = state->bound.columns;
    state->bound.columns = file_sql::describe_columns();
  }
  if (OB_FAIL(ret)) LOG_WARN("cannot open file table",K(ret),"detail",error.c_str());
  return ret;
}
int ObFunctionTableOp::next_file_row()
{
  int ret = OB_SUCCESS;
  auto *state = static_cast<FileScanState *>(file_state_);
  if (state == nullptr) return OB_ERR_UNEXPECTED;
  if (OB_FAIL(ctx_.check_status())) return ret;
  clear_evaluated_flag();
  if (state->bound.describe) {
    if (state->position >= state->description.size()) return OB_ITER_END;
    const auto &c = state->description[state->position++];
    state->cells = {{c.name,false},{file_sql::type_name(c.type),false},{c.nullable ? "YES" : "NO",false},
                    {"",true},{"",true},{"",true}};
  } else if (state->bound.discover) {
    if (state->position >= state->files.size()) return OB_ITER_END;
    state->cells = {{state->files[state->position++],false}};
  } else if (!state->reader->next(state->cells)) {
    if (state->reader->failed()) {
      LOG_WARN("file table scan failed","detail",state->reader->error().c_str());
      return OB_INVALID_ARGUMENT;
    }
    return OB_ITER_END;
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < MY_SPEC.column_exprs_.count(); ++i) {
    const int64_t index = MY_SPEC.file_column_ids_.at(i);
    if (index < 0 || index >= static_cast<int64_t>(state->cells.size())) return OB_ERR_UNEXPECTED;
    const auto &cell = state->cells[index];
    const auto type = state->bound.columns[index].type;
    ObObj value;
    if (cell.null) value.set_null();
    else {
      int64_t integer=0; double number=0; bool dt=false;
      switch (type) {
        case file_sql::Type::Int:
          if (!file_sql::int_value(cell.text,integer)) ret=OB_INVALID_ARGUMENT;
          else value.set_int(integer);
          break;
        case file_sql::Type::Double:
          if (!file_sql::double_value(cell.text,number)) ret=OB_INVALID_ARGUMENT;
          else value.set_double(number);
          break;
        case file_sql::Type::Bool: value.set_tinyint(cell.text=="true" || cell.text=="TRUE"); break;
        case file_sql::Type::Date:
        case file_sql::Type::DateTime:
          if (!file_sql::temporal(cell.text,dt,integer)) ret=OB_INVALID_ARGUMENT;
          else if (type==file_sql::Type::Date) value.set_date(static_cast<int32_t>(integer/86400000000LL));
          else value.set_datetime(integer);
          break;
        default:
          value.set_varchar(ObString(static_cast<int32_t>(cell.text.size()),cell.text.data()));
          value.set_collation_type(CS_TYPE_UTF8MB4_BIN);
          break;
      }
    }
    if (OB_SUCC(ret)) {
      ObExpr *expr = MY_SPEC.column_exprs_.at(i);
      OZ (expr->locate_datum_for_write(eval_ctx_).from_obj(value,expr->obj_datum_map_));
      if (OB_SUCC(ret)) expr->set_evaluated_projected(eval_ctx_);
    }
  }
  return ret;
}

int ObFunctionTableOp::inner_open()
{
  int ret = OB_SUCCESS;
  if (MY_SPEC.file_scan_) { return open_file_scan(); }
  node_idx_ = 0;
  already_calc_ = false;
  if (OB_ISNULL(MY_SPEC.value_expr_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (ObExtendType == MY_SPEC.value_expr_->datum_meta_.type_) {
    next_row_func_ = &ObFunctionTableOp::inner_get_next_row_udf;
  } else {
    next_row_func_ = &ObFunctionTableOp::inner_get_next_row_sys_func;
  }
  return ret;
}

int ObFunctionTableOp::inner_rescan()
{
  if (MY_SPEC.file_scan_) { return open_file_scan(); }
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObOperator::inner_rescan())) {
  } else {
    node_idx_ = 0;
    if (MY_SPEC.has_correlated_expr_) {
      row_count_ = 0;
      col_count_ = 0;
      value_table_ = NULL;
      already_calc_ = false;
    }
  }
  return ret;
}

int ObFunctionTableOp::inner_close()
{
  close_file_scan();
  int ret = OB_SUCCESS;
  node_idx_ = 0;
  row_count_ = 0;
  col_count_ = 0;
  value_table_ = NULL;
  return ret;
}

void ObFunctionTableOp::destroy()
{
  close_file_scan();
  ObOperator::destroy();
}


int ObFunctionTableOp::get_current_result(ObObj &result)
{
  int ret = OB_SUCCESS;
  void *data = NULL;
  CK (already_calc_);
  if (node_idx_ < 0 || node_idx_ >= row_count_ * col_count_) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("fail to get current result in table function",
              K(node_idx_), K(row_count_), K(col_count_));
  }
  do {
    CK (node_idx_ >= 0);
    if (OB_SUCC(ret) && node_idx_ >= row_count_) {
      ret = OB_ITER_END;
    }
    CK (OB_NOT_NULL(value_table_));
    OX (data = value_table_->get_data());
    CK (OB_NOT_NULL(data));
    OX (result = (static_cast<ObObj*>(data))[node_idx_++]);
  } while (OB_SUCC(ret) && result.get_meta().get_type() == ObMaxType);
  return ret;
}

int ObFunctionTableOp::inner_get_next_row()
{
  if (MY_SPEC.file_scan_) { return next_file_row(); }
  return (this->*next_row_func_)();
}

int ObFunctionTableOp::inner_get_next_row_udf()
{
  int ret = OB_SUCCESS;
  ObPhysicalPlanCtx *plan_ctx = nullptr;
  clear_evaluated_flag();
  if (OB_ISNULL(plan_ctx = ctx_.get_physical_plan_ctx())) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(MY_SPEC.value_expr_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (ObExtendType != MY_SPEC.value_expr_->datum_meta_.type_) {
    ret = OB_ERR_UNEXPECTED;
  } else if (FALSE_IT(plan_ctx->set_autoinc_id_tmp(0))) {
  } else if (OB_FAIL(ctx_.check_status())) {
  } else {
    ObDatum *value = nullptr;
    if (!already_calc_) {
      if (OB_FAIL(MY_SPEC.value_expr_->eval(eval_ctx_, value))) {
      } else if (value->is_null()) {
        //do nothing
      } else if (OB_ISNULL(value_table_ 
                 = reinterpret_cast<pl::ObPLCollection*>(value->get_ext()))) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        row_count_ = value_table_->is_inited() ? value_table_->get_count() : 0;
        col_count_ = value_table_->get_column_count();
      }
      already_calc_ = true;
    }
    if (OB_SUCC(ret) && OB_UNLIKELY(node_idx_ >= row_count_)) {
      ret = OB_ITER_END;
    }
    CK (MY_SPEC.column_exprs_.count() >= col_count_);
    ObObj obj_stack[col_count_];
    if (OB_SUCC(ret)) {
      if (nullptr != value_table_ 
          && ObExtendType == value_table_->get_element_type().get_obj_type()) {
        pl::ObPLComposite *composite = NULL;
        pl::ObPLRecord *record = NULL;
        ObObj record_obj;
        OZ (get_current_result(record_obj));
        if (OB_FAIL(ret)) {
        } else if (ObUserDefinedSQLType == record_obj.get_type()) {
          obj_stack[0] = record_obj;   
        } else if (record_obj.is_pl_extend()) {
          CK (OB_NOT_NULL(composite = reinterpret_cast<pl::ObPLComposite*>(record_obj.get_ext())));
          if (OB_SUCC(ret)) {
            if (composite->is_record()) {
              OX (record = static_cast<pl::ObPLRecord*>(composite));
              CK (record->get_count() == col_count_);
              for (int64_t i = 0; OB_SUCC(ret) && i < col_count_; ++i) {
                OZ (record->get_element(i, obj_stack[i]));
              }
            } else if (composite->is_collection()) {
              OX (obj_stack[0] = record_obj);
            } else {
              ret = OB_ERR_UNEXPECTED;
            }
          }
        } else {
          ret = OB_ERR_UNEXPECTED;
          LOG_ERROR("unexpected here", K(ret), K(record_obj), K(record_obj.meta_));
        }
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < col_count_; ++i) {
          if (OB_FAIL(get_current_result(obj_stack[i]))) {
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      for (int64_t i = 0; OB_SUCC(ret) && i < col_count_; ++i) {
        if (obj_stack[i].is_null()) {
          MY_SPEC.column_exprs_.at(i)->locate_datum_for_write(eval_ctx_).set_null();
        } else {
          const ObObjDatumMapType &datum_map = MY_SPEC.column_exprs_.at(i)->obj_datum_map_;
          ObExpr * const &expr = MY_SPEC.column_exprs_.at(i);
          ObDatum &datum = expr->locate_datum_for_write(eval_ctx_);
          if (OB_FAIL(datum.from_obj(obj_stack[i], datum_map))) {
          } else if (is_lob_storage(obj_stack[i].get_type()) &&
                     OB_FAIL(ob_adjust_lob_datum(get_exec_ctx(), obj_stack[i],
                                                 expr->obj_meta_, datum_map,
                                                 get_exec_ctx().get_allocator(), datum))) {
          }
        }
        if (OB_SUCC(ret)) {
          MY_SPEC.column_exprs_.at(i)->set_evaluated_projected(eval_ctx_);
        }
      }
    }
  }
  return ret;
}

int ObFunctionTableOp::inner_get_next_row_sys_func()
{
  int ret = OB_SUCCESS;
  ObPhysicalPlanCtx *plan_ctx = nullptr;
  ObDatum *value = nullptr;
  clear_evaluated_flag();
  if (OB_ISNULL(plan_ctx = ctx_.get_physical_plan_ctx())) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(ctx_.check_status())) {
  } else if (OB_FAIL(MY_SPEC.value_expr_->eval(eval_ctx_, value))) {
    if (OB_ITER_END != ret) {
    }
  } else {
    MY_SPEC.column_exprs_.at(0)->locate_datum_for_write(eval_ctx_).set_datum(*value);
    MY_SPEC.column_exprs_.at(0)->set_evaluated_projected(eval_ctx_);
  }
  return ret;
}


} // end namespace sql
} // end namespace oceanbase
