// Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
#pragma once
#include "query/session/ob_inner_sql_connection_access.h"
#include "sql/session/ob_inner_sql_connection.h"
namespace oceanbase {
namespace sql {
// Nested SQL preserves the caller's transaction and statement bookkeeping.
class AiColumnSql {
  common::sqlclient::ObISQLConnectionGuard connection_;
  ObSQLSessionInfo::StmtSavedValue session_value_;
  ObIInnerSQLConnection::SavedValue connection_value_;
  ObIInnerSQLConnection *inner_ = nullptr;
  bool nested_ = false;
  ObSQLSessionInfo *session_ = nullptr;
  bool installed_ = false;

public:
  ~AiColumnSql() {
    if (nested_)
      inner_->end_nested_session(session_value_, connection_value_);
    if (installed_) session_->set_inner_conn(nullptr);
  }
  int init(ObSQLSessionInfo *session) {
    int ret = common::OB_SUCCESS;
    session_ = session;
    if (!session) return common::OB_INVALID_ARGUMENT;
    if (session->get_inner_conn()) {
      inner_ = as_inner_sql_connection(session->get_inner_conn());
    } else {
      ret = query::ObInnerSQLConnectionAccess::
          create_connection_with_external_session(session, connection_);
      if (ret == common::OB_SUCCESS) {
        inner_ = as_inner_sql_connection(connection_.get_ptr());
        if (inner_) {
          session->set_inner_conn(inner_);
          installed_ = true;
        }
      }
    }
    if (ret == common::OB_SUCCESS) {
      if (!inner_) ret = common::OB_ERR_UNEXPECTED;
      else {
        ret = inner_->begin_nested_session(session_value_, connection_value_, false);
        nested_ = ret == common::OB_SUCCESS;
      }
    }
    return ret;
  }
  common::sqlclient::ObISQLConnection *get() { return inner_; }
  int finish(int ret) {
    if (nested_) {
      nested_ = false;
      int rc = inner_->end_nested_session(session_value_, connection_value_);
      if (ret == common::OB_SUCCESS)
        ret = rc;
    }
    return ret;
  }
};
} // namespace sql
} // namespace oceanbase
