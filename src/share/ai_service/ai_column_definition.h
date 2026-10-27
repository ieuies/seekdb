// Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
#pragma once
#include <cstdint>
#include <cctype>
#include <string>
#include <vector>
#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"
namespace oceanbase { namespace share {
inline std::vector<std::string> ai_sql_arguments(const std::string &expression) {
  std::vector<std::string> arguments;
  size_t begin=expression.find('(');
  if(begin==std::string::npos)return arguments;
  int depth=1;char quote=0;size_t start=++begin;
  for(size_t i=begin;i<expression.size();++i){char c=expression[i];
    if(quote){if(c=='\\' && i+1<expression.size())++i;else if(c==quote){if(i+1<expression.size()&&expression[i+1]==quote)++i;else quote=0;}continue;}
    if(c=='\''||c=='"'||c=='`')quote=c;
    else if(c=='(')++depth;
    else if(c==')'){if(!--depth){arguments.push_back(expression.substr(start,i-start));return arguments;}}
    else if(c==','&&depth==1){arguments.push_back(expression.substr(start,i-start));start=i+1;}
  }
  arguments.clear();return arguments;
}
struct AiColumnDefinition {
  std::string expression;
  std::string normalized_expression;
  std::vector<std::string> dependencies;
  std::vector<uint64_t> dependency_ids;
  uint64_t state_column_id=0,request_column_id=0,result_spec_column_id=0;
  uint64_t definition_version=1;
  int max_retries=2;
  int vector_dimension=0;
  std::string encode() const {
    rapidjson::StringBuffer b;rapidjson::Writer<rapidjson::StringBuffer> w(b);w.StartObject();
    w.Key("version");w.Uint(1);
    w.Key("expression");w.String(expression.data(),static_cast<rapidjson::SizeType>(expression.size()));
    w.Key("normalized_expr");w.String(normalized_expression.data(),static_cast<rapidjson::SizeType>(normalized_expression.size()));
    w.Key("max_retries");w.Int(max_retries);w.Key("sync_mode");w.String("async");
    w.Key("dimension");w.Int(vector_dimension);w.Key("definition_version");w.Uint64(definition_version);
    w.Key("state_column_id");w.Uint64(state_column_id);w.Key("request_column_id");w.Uint64(request_column_id);w.Key("result_spec_column_id");w.Uint64(result_spec_column_id);
    w.Key("dependencies");w.StartArray();for(const auto &d:dependencies)w.String(d.data(),static_cast<rapidjson::SizeType>(d.size()));w.EndArray();
    w.Key("dependency_ids");w.StartArray();for(auto d:dependency_ids)w.Uint64(d);w.EndArray();w.EndObject();return std::string(b.GetString(),b.GetSize());
  }
  static bool decode(const std::string &value,AiColumnDefinition &out) {
    rapidjson::Document d;d.Parse(value.data(),value.size());
    if(d.HasParseError()||!d.IsObject()||!d.HasMember("version")||!d["version"].IsUint()||d["version"].GetUint()!=1)return false;
    for(const char *key:{"expression","normalized_expr"})if(!d.HasMember(key)||!d[key].IsString())return false;
    for(const char *key:{"state_column_id","request_column_id","result_spec_column_id","definition_version"})if(!d.HasMember(key)||!d[key].IsUint64())return false;
    if(!d.HasMember("max_retries")||!d["max_retries"].IsInt()||d["max_retries"].GetInt()<0||d["max_retries"].GetInt()>100)return false;
    if(!d.HasMember("dimension")||!d["dimension"].IsInt()||!d.HasMember("dependencies")||!d["dependencies"].IsArray()||!d.HasMember("dependency_ids")||!d["dependency_ids"].IsArray())return false;
    AiColumnDefinition next;next.expression.assign(d["expression"].GetString(),d["expression"].GetStringLength());next.normalized_expression.assign(d["normalized_expr"].GetString(),d["normalized_expr"].GetStringLength());
    next.max_retries=d["max_retries"].GetInt();next.vector_dimension=d["dimension"].GetInt();next.definition_version=d["definition_version"].GetUint64();
    next.state_column_id=d["state_column_id"].GetUint64();next.request_column_id=d["request_column_id"].GetUint64();next.result_spec_column_id=d["result_spec_column_id"].GetUint64();
    for(auto &x:d["dependencies"].GetArray()){if(!x.IsString())return false;next.dependencies.emplace_back(x.GetString(),x.GetStringLength());}
    for(auto &x:d["dependency_ids"].GetArray()){if(!x.IsUint64())return false;next.dependency_ids.push_back(x.GetUint64());}
    out=std::move(next);return true;
  }
};
}}
