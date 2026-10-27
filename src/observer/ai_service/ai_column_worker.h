// Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
#pragma once
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "common/mysqlclient/ob_mysql_transaction.h"
#include "share/ob_server_struct.h"
#include "share/ob_thread_pool.h"
#include "sql/engine/expr/ob_expr_uuid.h"
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <map>
#include <rapidjson/document.h>
#include <string>
#include <vector>
namespace oceanbase {
namespace observer {
// States: queued, leased, success staged, retry delayed, failure staged.
// Publish a group in one transaction only after all its tasks are terminal.
class AiColumnWorker : public share::ObThreadPool {
  using Client = common::ObISQLClient;
  struct Task {
    std::string request, predicate, table, expression, column, state_column,
        request_column, spec_column, spec, value;
    int64_t column_id = 0, table_id = 0, version = 0, retries = 0,
            max_retries = 0, state = 0;
  };
  static std::string hex(const std::string &s) {
    const char digits[] = "0123456789abcdef";
    std::string out = "X'";
    for (unsigned char c : s) {
      out += digits[c >> 4];
      out += digits[c & 15];
    }
    return out + "'";
  }
  static std::string identifier(const std::string &s) {
    std::string out = "`";
    for (char c : s) {
      if (c == '`')
        out += '`';
      out += c;
    }
    return out + '`';
  }
  static std::string key(const Task &t) {
    return "request_id=" + hex(t.request) +
           " AND column_id=" + std::to_string(t.column_id);
  }
  static int write(Client &client, const std::string &sql,
                   int64_t *affected = nullptr) {
    int64_t n = 0;
    int ret = client.write(sql.c_str(), n);
    if (affected)
      *affected = n;
    return ret;
  }
  static int load(Client &client, const std::string &suffix,
                  std::vector<Task> &tasks) {
    using namespace common;
    int ret = OB_SUCCESS;
    Client::ReadResult result;
    std::string query =
        "SELECT "
        "request_id,column_id,table_id,definition_version,rowkey_predicate,"
        "qualified_table_name,"
        "CAST(normalized_expr AS CHAR) AS "
        "expression,result_column_name,state_column_name,request_column_name,"
        "result_spec_column_name,CAST(result_spec AS CHAR) AS "
        "spec,state,retry_count,max_retries,"
        "CAST(COALESCE(result_value,'') AS CHAR) AS value FROM "
        "oceanbase.__all_ai_column_task " +
        suffix;
    if (OB_FAIL(client.read(result, query.c_str())))
      return ret;
    auto *r = result.get_result();
    if (!r)
      return OB_ERR_UNEXPECTED;
    while (OB_SUCC(ret) && OB_SUCC(ret = r->next())) {
      Task t;
      ObString value;
      auto text = [&](const char *name, std::string &out) {
        int rc = r->get_varchar(name, value);
        if (rc == OB_SUCCESS)
          out.assign(value.ptr(), value.length());
        return rc;
      };
      OZ(text("request_id", t.request));
      OZ(r->get_int("column_id", t.column_id));
      OZ(r->get_int("table_id", t.table_id));
      OZ(r->get_int("definition_version", t.version));
      OZ(text("rowkey_predicate", t.predicate));
      OZ(text("qualified_table_name", t.table));
      OZ(text("expression", t.expression));
      OZ(text("result_column_name", t.column));
      OZ(text("state_column_name", t.state_column));
      OZ(text("request_column_name", t.request_column));
      OZ(text("result_spec_column_name", t.spec_column));
      OZ(text("spec", t.spec));
      OZ(r->get_int("state", t.state));
      OZ(r->get_int("retry_count", t.retries));
      OZ(r->get_int("max_retries", t.max_retries));
      OZ(text("value", t.value));
      if (OB_SUCC(ret))
        tasks.push_back(std::move(t));
    }
    return ret == OB_ITER_END ? OB_SUCCESS : ret;
  }
  static bool validate(const std::string &value, const std::string &spec,
                       std::string &literal) {
    if (value.size() > 8 * 1024 * 1024)
      return false;
    if (spec == "SIGNED") {
      int64_t n = 0;
      auto parsed =
          std::from_chars(value.data(), value.data() + value.size(), n);
      if (parsed.ec != std::errc() || parsed.ptr != value.data() + value.size())
        return false;
      literal = std::to_string(n);
    } else if (spec == "DOUBLE") {
      char *end = nullptr;
      double n = std::strtod(value.c_str(), &end);
      if (value.empty() || end != value.c_str() + value.size() ||
          !std::isfinite(n))
        return false;
      literal = value;
    } else if (spec == "JSON" || spec.compare(0, 7, "VECTOR(") == 0) {
      rapidjson::Document json;
      json.Parse<rapidjson::kParseValidateEncodingFlag>(value.data(), value.size());
      if (json.HasParseError())
        return false;
      if (spec != "JSON") {
        unsigned dimension = 0;
        auto parsed = std::from_chars(spec.data() + 7,
                                      spec.data() + spec.size() - 1, dimension);
        if (parsed.ec != std::errc() || !json.IsArray() ||
            json.Size() != dimension)
          return false;
        for (auto &entry : json.GetArray())
          if (!entry.IsNumber() || !std::isfinite(entry.GetDouble()))
            return false;
      }
      literal = spec == "JSON"
          ? "CAST(CONVERT(" + hex(value) + " USING utf8mb4) AS JSON)"
          : value; // Validated numeric arrays use the native vector literal syntax.
    } else
      literal = "CONVERT(" + hex(value) + " USING utf8mb4)";
    return true;
  }
  static std::string current_definition(const Task &t) {
    return "EXISTS (SELECT 1 FROM oceanbase.__all_column WHERE table_id=" +
           std::to_string(t.table_id) +
           " AND column_id=" + std::to_string(t.column_id) +
           " AND "
           "CAST(JSON_UNQUOTE(JSON_EXTRACT(NULLIF(ai_column_info,''),'$."
           "definition_version')) AS UNSIGNED)=" +
           std::to_string(t.version) + ")";
  }
  static int still_owned(Client &client, const Task &t, bool &owned) {
    using namespace common;
    owned = false;
    Client::ReadResult result;
    std::string query = "SELECT 1 FROM " + t.table + " WHERE (" + t.predicate +
                        ") AND " + identifier(t.request_column) + "=" +
                        hex(t.request) + " AND " + identifier(t.spec_column) +
                        "=" + hex(std::to_string(t.version)) + " AND " +
                        current_definition(t) + " LIMIT 1";
    int ret = client.read(result, query.c_str());
    if (ret == OB_TABLE_NOT_EXIST || ret == OB_ERR_BAD_FIELD_ERROR)
      return OB_SUCCESS;
    if (ret != OB_SUCCESS)
      return ret;
    if (!result.get_result())
      return OB_ERR_UNEXPECTED;
    ret = result.get_result()->next();
    owned = ret == OB_SUCCESS;
    return ret == OB_ITER_END ? OB_SUCCESS : ret;
  }
  static int publish(const Task &task) {
    using namespace common;
    int ret = OB_SUCCESS;
    ObMySQLTransaction tx;
    std::vector<Task> group;
    OZ(tx.start(GCTX.sql_proxy_));
    OZ(load(tx,
            "WHERE request_id=" + hex(task.request) +
                " ORDER BY column_id FOR UPDATE",
            group));
    for (const auto &t : group)
      if (t.state != 2 && t.state != 4) {
        if (tx.is_started())
          tx.end(false);
        return OB_SUCCESS;
      }
    for (const auto &t : group) {
      if (OB_FAIL(ret))
        break;
      bool owned = false;
      OZ(still_owned(tx, t, owned));
      if (OB_FAIL(ret)) break;
      if (!owned) {
        // A handled missing table/column read leaves the connection proxy's
        // sticky error set. Clear it before deleting this obsolete task group.
        tx.reset_last_error();
        continue;
      }
      std::string literal = "NULL";
      bool success = t.state == 2 && validate(t.value, t.spec, literal);
      std::string query =
          "UPDATE " + t.table + " SET " + identifier(t.column) + "=" +
          (success ? literal : "NULL") + "," + identifier(t.state_column) +
          "='" + (success ? "READY" : "FAILED") + "' WHERE (" + t.predicate +
          ") AND " + identifier(t.request_column) + "=" + hex(t.request) +
          " AND " + identifier(t.spec_column) + "=" +
          hex(std::to_string(t.version)) + " AND " + current_definition(t);
      ret = write(tx, query);
      if(ret==OB_ERR_DATA_TOO_LONG||ret==OB_DATA_OUT_OF_RANGE||
         ret==OB_ERR_TRUNCATED_WRONG_VALUE_FOR_FIELD||ret==OB_ERR_TRUNCATED_WRONG_VALUE||
         ret==OB_ERR_INVALID_JSON_TEXT||ret==OB_ERR_INVALID_VECTOR_DIM){
        // A native target conversion must not leave a staged group stuck.
        // Roll back every result, then persist this column's terminal failure;
        // a later publication still commits the whole group together.
        const int conversion_error=ret;
        if(tx.is_started()){int rollback=tx.end(false);if(rollback!=OB_SUCCESS)return rollback;}
        return write(*GCTX.sql_proxy_,"UPDATE oceanbase.__all_ai_column_task SET state=4,last_error="+
          std::to_string(conversion_error)+" WHERE "+key(t)+" AND state=2");
      }
      if (ret == OB_TABLE_NOT_EXIST || ret == OB_ERR_BAD_FIELD_ERROR) {
        tx.reset_last_error();
        ret = OB_SUCCESS;
      }
    }
    OZ(write(tx,
             "DELETE FROM oceanbase.__all_ai_column_task WHERE request_id=" +
                 hex(task.request)));
    if (tx.is_started()) {
      int rc = tx.end(OB_SUCC(ret));
      if (OB_SUCC(ret))
        ret = rc;
    }
    return ret;
  }
  int step() {
    using namespace common;
    int ret = OB_SUCCESS;
    std::vector<Task> tasks;
    auto &proxy = *GCTX.sql_proxy_;
    OZ(load(proxy, "WHERE state IN (2,4) ORDER BY gmt_create LIMIT 1", tasks));
    if (!tasks.empty()) {
      OZ(publish(tasks.front()));
      tasks.clear();
    }
    OZ(load(proxy,
            "WHERE (state IN (0,3) AND eligible_ts<=NOW(6)) OR (state=1 AND "
            "lease_until<NOW(6)) ORDER BY gmt_create LIMIT 1",
            tasks));
    if (OB_FAIL(ret) || tasks.empty())
      return OB_FAIL(ret) ? ret : OB_ITER_END;
    Task t = std::move(tasks.front());
    char uuid[sql::UuidCommon::LENGTH_UUID] = {0};
    OZ(sql::ObExprUuid::gen_server_uuid(uuid, sizeof(uuid)));
    std::string owner(uuid, sizeof(uuid));
    int64_t claimed = 0;
    OZ(write(proxy,
             "UPDATE oceanbase.__all_ai_column_task SET state=1,lease_owner=" +
                 hex(owner) +
                 ",lease_until=DATE_ADD(NOW(6),INTERVAL 30 SECOND) WHERE " +
                 key(t) +
                 " AND ((state IN (0,3) AND eligible_ts<=NOW(6)) OR (state=1 "
                 "AND lease_until<NOW(6)))",
             &claimed));
    if (OB_FAIL(ret) || claimed != 1)
      return ret;
    bool owned = false;
    OZ(still_owned(proxy, t, owned));
    if (OB_FAIL(ret))
      return ret;
    if (!owned) {
      OZ(write(proxy,
               "UPDATE oceanbase.__all_ai_column_task SET state=4 WHERE " +
                   key(t) + " AND lease_owner=" + hex(owner)));
      OZ(publish(t));
      return ret;
    }
    int model_ret = OB_SUCCESS;
    std::string response, literal;
    {
      Client::ReadResult result;
      std::string query = "SELECT CAST(ai_result AS CHAR) AS value FROM (" +
                          t.expression + ") AS ai_response";
      model_ret = proxy.read(result, query.c_str());
      auto *r = result.get_result();
      ObString value;
      if (model_ret == OB_SUCCESS)
        model_ret = r ? r->next() : OB_ERR_UNEXPECTED;
      if (model_ret == OB_SUCCESS)
        model_ret = r->get_varchar("value", value);
      if (model_ret == OB_SUCCESS)
        response.assign(value.ptr(), value.length());
    }
    if (model_ret == OB_SUCCESS && !validate(response, t.spec, literal))
      model_ret = OB_INVALID_ARGUMENT;
    std::string change;
    if (model_ret == OB_SUCCESS)
      change = "state=2,result_value=" + hex(response);
    else if (t.retries < t.max_retries)
      change = "state=3,retry_count=retry_count+1,eligible_ts=DATE_ADD(NOW(6),"
               "INTERVAL 500000 MICROSECOND)";
    else
      change = "state=4";
    change += ",last_error=" + std::to_string(model_ret) +
              ",lease_owner=NULL,lease_until=NULL";
    OZ(write(proxy, "UPDATE oceanbase.__all_ai_column_task SET " + change +
                        " WHERE " + key(t) +
                        " AND state=1 AND lease_owner=" + hex(owner)));
    OZ(publish(t));
    return ret;
  }
  static int backfill() {
    using namespace common;
    int ret = OB_SUCCESS;
    Client::ReadResult result;
    const char *query =
        "SELECT a.table_id,d.database_name,t.table_name,r.column_name AS "
        "request_name,"
        "s.column_name AS spec_name,a.definition_version FROM "
        "oceanbase.__all_ai_column a "
        "JOIN oceanbase.__all_table t ON t.table_id=a.table_id "
        "JOIN oceanbase.__all_database d ON d.database_id=t.database_id "
        "JOIN oceanbase.__all_column r ON r.table_id=a.table_id AND "
        "r.column_id=a.request_column_id "
        "JOIN oceanbase.__all_column s ON s.table_id=a.table_id AND "
        "s.column_id=a.result_spec_column_id";
    OZ(GCTX.sql_proxy_->read(result, query));
    struct Update {
      std::string table, assignments, condition;
    };
    std::map<int64_t, Update> updates;
    auto *row = result.get_result();
    if (OB_SUCC(ret) && !row)
      return OB_ERR_UNEXPECTED;
    while (OB_SUCC(ret) && OB_SUCC(ret = row->next())) {
      int64_t table_id = 0, version = 0;
      ObString value;
      std::string database, table, request, spec;
      auto text = [&](const char *name, std::string &out) {
        int rc = row->get_varchar(name, value);
        if (rc == OB_SUCCESS)
          out.assign(value.ptr(), value.length());
        return rc;
      };
      OZ(row->get_int("table_id", table_id));
      OZ(row->get_int("definition_version", version));
      OZ(text("database_name", database));
      OZ(text("table_name", table));
      OZ(text("request_name", request));
      OZ(text("spec_name", spec));
      if (OB_SUCC(ret)) {
        auto &update = updates[table_id];
        update.table = identifier(database) + "." + identifier(table);
        std::string condition = "(" + identifier(spec) + " IS NULL OR " +
                                identifier(spec) + "<>" +
                                hex(std::to_string(version)) + ")";
        if (!update.assignments.empty()) {
          update.assignments += ",";
          update.condition += " OR ";
        }
        update.assignments += identifier(request) + "=CASE WHEN " + condition +
                              " THEN '__ai_retry__' ELSE " +
                              identifier(request) + " END";
        update.condition += condition;
      }
    }
    if (ret == OB_ITER_END)
      ret = OB_SUCCESS;
    result.close();
    for (const auto &entry : updates) {
      if (OB_FAIL(ret))
        break;
      const auto &u = entry.second;
      ret = write(*GCTX.sql_proxy_, "UPDATE " + u.table + " SET " +
                                        u.assignments + " WHERE " +
                                        u.condition + " LIMIT 64");
    }
    return ret;
  }
  void run1() override {
    lib::set_thread_name("AiColumnWorker");
    while (!has_set_stop()) {
      int ret = common::OB_SUCCESS;
      if (GCTX.sql_proxy_ && GCTX.status_ == share::SS_SERVING)
        ret = step();
      if (ret == common::OB_ITER_END) {
        static std::atomic<int64_t> next_backfill{0};
        int64_t due = next_backfill.load(),
                now = common::ObTimeUtility::current_time();
        if (now >= due &&
            next_backfill.compare_exchange_strong(due, now + 1000000))
          ret = backfill();
      }
      if (ret != common::OB_SUCCESS && ret != common::OB_ITER_END &&
          ret != common::OB_TABLE_NOT_EXIST)
        LOG_WARN("AI column worker step failed", K(ret));
      common::ob_usleep(ret == common::OB_ITER_END ? 200000 : 50000);
    }
  }

public:
  AiColumnWorker() : share::ObThreadPool(2) {}
  static AiColumnWorker &instance() {
    static AiColumnWorker worker;
    return worker;
  }
};
} // namespace observer
} // namespace oceanbase
