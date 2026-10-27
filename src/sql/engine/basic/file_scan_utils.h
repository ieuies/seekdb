// Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <cctype>
#include <cwctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <string>
#include <vector>
#include <sys/stat.h>
#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"
#ifdef _WIN32
#include <io.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace oceanbase { namespace sql { namespace file_sql {
enum class Type { Null, Int, Double, Bool, String, Date, DateTime };
struct Cell { std::string text; bool null = true; Type hint = Type::Null; };
struct Column { std::string name; Type type = Type::Null; bool nullable = false; int scale = 0; };
struct Options {
  std::string path, format = "csv", nullstr;
  char delimiter = ',', quote = '"', escape = '"';
  bool header = true;
};
struct Stamp {
  uint64_t device = 0, inode = 0, size = 0;
  int64_t mtime = 0, nanos = 0;
  bool operator==(const Stamp &s) const {
    return device == s.device && inode == s.inode && size == s.size && mtime == s.mtime && nanos == s.nanos;
  }
};
inline bool stamp_file(FILE *f, Stamp &s) {
#ifdef _WIN32
  struct _stat64 b;
  if (_fstat64(_fileno(f), &b)) return false;
#else
  struct stat b;
  if (fstat(fileno(f), &b)) return false;
#if defined(__APPLE__)
  s.nanos = b.st_mtimespec.tv_nsec;
#else
  s.nanos = b.st_mtim.tv_nsec;
#endif
#endif
  if (!(b.st_mode & S_IFREG)) return false;
  s.device = b.st_dev; s.inode = b.st_ino; s.size = b.st_size; s.mtime = b.st_mtime;
  return true;
}
inline bool valid_utf8(const std::string &s) {
  uint32_t cp = 0, minimum = 0; int left = 0;
  for (unsigned char c : s) {
    if (!left) {
      if (c < 128) continue;
      if (c >= 0xc2 && c <= 0xdf) { left = 1; cp = c & 31; minimum = 0x80; }
      else if (c >= 0xe0 && c <= 0xef) { left = 2; cp = c & 15; minimum = 0x800; }
      else if (c >= 0xf0 && c <= 0xf4) { left = 3; cp = c & 7; minimum = 0x10000; }
      else return false;
    } else {
      if ((c & 0xc0) != 0x80) return false;
      cp = (cp << 6) | (c & 63);
      if (!--left && (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))) return false;
    }
  }
  return left == 0;
}
inline bool int_value(const std::string &s, int64_t &v) {
  if (s.empty()) return false;
  const char *p = s.data(); if (*p == '+') ++p;
  if (p == s.data() + s.size()) return false;
  auto r = std::from_chars(p, s.data() + s.size(), v);
  return r.ec == std::errc() && r.ptr == s.data() + s.size();
}
inline bool double_value(const std::string &s, double &v) {
  if (s.empty() || s.front() == ' ' || s.back() == ' ') return false;
  char *end = nullptr; errno = 0; v = std::strtod(s.c_str(), &end);
  return errno != ERANGE && end == s.c_str() + s.size() && std::isfinite(v);
}
inline bool temporal(const std::string &s, bool &dt, int64_t &micros) {
  dt = s.size() > 10;
  if (s.size() != 10 && s.size() != 19 && !(s.size() >= 21 && s.size() <= 26)) return false;
  auto num = [&](size_t pos, size_t len, int &v) {
    v = 0; for (size_t i = pos; i < pos + len; ++i) {
      if (s[i] < '0' || s[i] > '9') return false; v = v * 10 + s[i] - '0';
    } return true;
  };
  int y, m, d, h = 0, mi = 0, se = 0;
  if (s[4] != '-' || s[7] != '-' || !num(0,4,y) || !num(5,2,m) || !num(8,2,d)) return false;
  if (y < 1 || m < 1 || m > 12) return false;
  const int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  if (d < 1 || d > days[m-1] + (m == 2 && (y%4 == 0 && (y%100 != 0 || y%400 == 0)))) return false;
  if (dt && ((s[10] != 'T' && s[10] != ' ') || s[13] != ':' || s[16] != ':' ||
      !num(11,2,h) || !num(14,2,mi) || !num(17,2,se) || h > 23 || mi > 59 || se > 59)) return false;
  int us = 0;
  if (s.size() > 19) {
    if (s[19] != '.' || !num(20,s.size()-20,us)) return false;
    for (size_t i = s.size()-20; i < 6; ++i) us *= 10;
  }
  y -= m <= 2; const int era = (y >= 0 ? y : y-399) / 400;
  const unsigned yo = static_cast<unsigned>(y-era*400), mp = m > 2 ? m-3 : m+9;
  const unsigned doy = (153*mp+2)/5+d-1, doe = yo*365+yo/4-yo/100+doy;
  const int64_t epoch_days = era*146097 + static_cast<int64_t>(doe) - 719468;
  micros = ((epoch_days*24+h)*3600+mi*60+se)*1000000+us;
  return true;
}
inline Type detect(const Cell &c) {
  if (c.null) return Type::Null;
  if (c.hint != Type::Null) return c.hint;
  if (c.text == "true" || c.text == "false" || c.text == "TRUE" || c.text == "FALSE") return Type::Bool;
  int64_t n; if (int_value(c.text,n)) return Type::Int;
  // Out-of-range integer literals stay text rather than silently losing precision.
  size_t start = (!c.text.empty() && (c.text[0]=='-' || c.text[0]=='+')) ? 1 : 0;
  if (start < c.text.size() && c.text.find_first_not_of("0123456789",start)==std::string::npos) return Type::String;
  double v; if (double_value(c.text,v)) return Type::Double;
  bool dt; if (temporal(c.text,dt,n)) return dt ? Type::DateTime : Type::Date;
  return Type::String;
}
inline Type widen(Type a, Type b) {
  if (a == Type::Null) return b;
  if (b == Type::Null || a == b) return a;
  if ((a == Type::Int && b == Type::Double) || (a == Type::Double && b == Type::Int)) return Type::Double;
  if ((a == Type::Date && b == Type::DateTime) || (a == Type::DateTime && b == Type::Date)) return Type::DateTime;
  return Type::String;
}
inline const char *type_name(Type t) {
  const char *names[] = {"NULL","BIGINT","DOUBLE","BOOLEAN","VARCHAR","DATE","DATETIME"};
  return names[static_cast<int>(t)];
}

}}}
#include "sql/engine/basic/file_parquet_reader.h"
namespace oceanbase { namespace sql { namespace file_sql {
// Bounded, two-pass reader. Schema inference retains only column metadata;
// rescans and execution reopen the source and verify its original identity.
class Reader {
public:
  static constexpr size_t MAX_RECORD = 8 * 1024 * 1024;
  static constexpr size_t MAX_COLUMNS = 1024;
  explicit Reader(const Options &o) : options_(o) {}
  ~Reader() { close(); }
  Reader(const Reader &) = delete;
  Reader &operator=(const Reader &) = delete;
  const std::string &error() const { return error_; }
  const std::vector<Column> &columns() const { return columns_; }
  const Stamp &stamp() const { return stamp_; }
  bool failed() const { return !error_.empty(); }
  uint64_t row_number() const { return row_; }
  bool fail(const std::string &message) { error_ = options_.path + ": record " + std::to_string(row_+1) + ": " + message; return false; }
  void close() { if (file_) { fclose(file_); file_ = nullptr; } }
  bool open(const Stamp *expected = nullptr) {
    close(); error_.clear(); row_ = 0;
    file_ = fopen(options_.path.c_str(), "rb");
    if (!file_) return fail("cannot open file");
    Stamp now;
    if (!stamp_file(file_,now) || (expected && !(now == *expected))) { close(); return fail("file changed or is not a regular file"); }
    stamp_ = now; eof_ = false;
    if (options_.format == "parquet") {
      std::vector<Column> actual;
      if (!parquet_.metadata(file_,stamp_.size,actual,error_)) return false;
      if (actual.size()!=columns_.size()) return fail("Parquet schema changed");
      for(size_t i=0;i<actual.size();++i) if(actual[i].name!=columns_[i].name || actual[i].type!=columns_[i].type) return fail("Parquet schema changed");
      if (projected_) parquet_.projection(projection_);
    }
    if (options_.format == "csv" && options_.header) {
      std::vector<Cell> discard;
      if (!csv(discard) && failed()) return false;
    }
    return true;
  }
  bool infer() {
    columns_.clear(); error_.clear();
    close(); file_ = fopen(options_.path.c_str(),"rb"); row_ = 0; eof_ = false;
    if (!file_ || !stamp_file(file_,stamp_)) return fail("cannot read a regular file");
    std::vector<Cell> cells;
    if (options_.format == "parquet") {
      if(!parquet_.metadata(file_,stamp_.size,columns_,error_)) return false;
    } else if (options_.format == "csv") {
      if (options_.header && csv(cells)) {
        for (size_t i = 0; i < cells.size(); ++i) add_column(cells[i].text.empty() ? "column"+std::to_string(i+1) : cells[i].text);
      }
      while (!failed() && csv(cells)) {
        if (columns_.empty()) for (size_t i=0;i<cells.size();++i) add_column("column"+std::to_string(i+1));
        if (cells.size() != columns_.size()) return fail("CSV field count differs from schema");
        merge(cells);
      }
    } else if (options_.format == "json") {
      while (!failed() && json(cells,true)) merge(cells);
    } else return fail("unsupported reader format");
    Stamp now;
    if (!stamp_file(file_,now) || !(now == stamp_)) return fail("file changed during schema inference");
    close(); return !failed();
  }
  void set_schema(const std::vector<Column> &columns) { columns_ = columns; }
  void projection(const std::vector<size_t> &p) { projection_=p; projected_=true; }
  bool next(std::vector<Cell> &cells) {
    if (!file_ || failed()) return false;
    if (row_ % 256 == 0) { Stamp now; if (!stamp_file(file_,now) || !(now == stamp_)) return fail("file changed during scan"); }
    bool ok = options_.format == "parquet" ? parquet_.next(cells,error_) : options_.format == "csv" ? csv(cells) : json(cells,false);
    if (ok && options_.format == "parquet") ++row_;
    if (ok && cells.size() != columns_.size()) return fail("field count differs from bound schema");
    if (!ok && !failed()) { Stamp now; if (!stamp_file(file_,now) || !(now == stamp_)) return fail("file changed during scan"); }
    return ok;
  }
private:
  size_t add_column(const std::string &name) {
    std::string unique = name; size_t n=1;
    auto exists = [&] { for (auto &c : columns_) {
      std::string a=c.name,b=unique; std::transform(a.begin(),a.end(),a.begin(),::tolower); std::transform(b.begin(),b.end(),b.begin(),::tolower);
      if (a==b) return true;
    } return false; };
    while (exists()) unique=name+"__"+std::to_string(++n);
    columns_.push_back({unique,Type::Null,row_ > 1}); return columns_.size()-1;
  }
  void merge(const std::vector<Cell> &cells) {
    for (size_t i=0;i<cells.size();++i) {
      columns_[i].nullable |= cells[i].null;
      const Type type = detect(cells[i]);
      columns_[i].type = widen(columns_[i].type, type);
      if (type == Type::DateTime) {
        const auto dot = cells[i].text.find('.', 19);
        if (dot != std::string::npos) columns_[i].scale = std::max(columns_[i].scale,
            static_cast<int>(cells[i].text.size() - dot - 1));
      }
    }
  }
  bool csv(std::vector<Cell> &cells) {
    cells.clear(); if (eof_) return false;
    Cell c; c.null=false; bool quoted=false, in_quote=false, after_quote=false, any=false; size_t bytes=0;
    auto field = [&] { if (!valid_utf8(c.text)) return fail("invalid UTF-8"); c.null=!quoted && c.text==options_.nullstr; cells.push_back(c); c=Cell(); c.null=false; quoted=false; after_quote=false; return cells.size()<=MAX_COLUMNS || fail("too many columns"); };
    for (;;) {
      int ch=fgetc(file_);
      if (ch == EOF) {
        eof_=true; if (ferror(file_)) return fail("read error");
        if (in_quote) return fail("unterminated quoted CSV field");
        if (!any && cells.empty() && c.text.empty()) return false;
        if (!field()) return false; ++row_; return true;
      }
      any=true; if (++bytes > MAX_RECORD) return fail("record exceeds 8 MiB limit");
      if (in_quote) {
        if (ch == options_.escape && options_.escape != options_.quote) {
          int e=fgetc(file_); if (e==EOF) return fail("incomplete escape");
          if(e==options_.quote||e==options_.escape)c.text.push_back(static_cast<char>(e));
          else {c.text.push_back(options_.escape);ungetc(e,file_);}
        } else if (ch == options_.quote) {
          int n=fgetc(file_);
          if (n == options_.quote && options_.escape==options_.quote) c.text.push_back(options_.quote);
          else { in_quote=false; after_quote=true; if(n!=EOF) ungetc(n,file_); }
        } else if (ch=='\r') { int n=fgetc(file_); if(n!='\n' && n!=EOF) ungetc(n,file_); c.text+='\n'; }
        else c.text.push_back(static_cast<char>(ch));
      } else if (ch == options_.delimiter) { if (!field()) return false; }
      else if (ch=='\n' || ch=='\r') {
        if(ch=='\r') { int n=fgetc(file_); if(n!='\n' && n!=EOF) ungetc(n,file_); }
        if (!field()) return false; ++row_; return true;
      } else if (ch==options_.quote && c.text.empty() && !after_quote) { quoted=true; in_quote=true; }
      else if (after_quote || ch==options_.quote) return fail("invalid character outside quoted field");
      else c.text.push_back(static_cast<char>(ch));
    }
  }
  bool json(std::vector<Cell> &cells, bool infer) {
    std::string line;
    for (;;) {
      int c=fgetc(file_); if(c==EOF) { if(ferror(file_)) return fail("read error"); if(line.empty()) return false; break; }
      if(c=='\n') { if(line.empty() || line=="\r") { line.clear(); continue; } break; }
      line.push_back(static_cast<char>(c)); if(line.size()>MAX_RECORD) return fail("record exceeds 8 MiB limit");
    }
    ++row_; rapidjson::Document doc;
    doc.Parse<rapidjson::kParseValidateEncodingFlag>(line.data(),line.size());
    if(doc.HasParseError() || !doc.IsObject()) return fail("JSONL requires a valid flat object");
    std::map<std::string,Cell> values;
    for(auto m=doc.MemberBegin();m!=doc.MemberEnd();++m) {
      std::string key(m->name.GetString(),m->name.GetStringLength());
      if(values.count(key)) return fail("duplicate JSON key");
      Cell c;
      if(m->value.IsArray() || m->value.IsObject()) return fail("nested JSON values are unsupported");
      if(!m->value.IsNull()) {
        c.null=false;
        if(m->value.IsString()) {
          c.text.assign(m->value.GetString(),m->value.GetStringLength());
          bool dt=false;int64_t time=0;
          c.hint=temporal(c.text,dt,time)?(dt?Type::DateTime:Type::Date):Type::String;
        }
        else { rapidjson::StringBuffer b; rapidjson::Writer<rapidjson::StringBuffer> w(b); m->value.Accept(w); c.text.assign(b.GetString(),b.GetSize()); }
      }
      values.emplace(key,std::move(c));
      if(infer) {
        bool found=false; for(auto &col:columns_) if(col.name==key) found=true;
        if(!found) { if(columns_.size()>=MAX_COLUMNS) return fail("too many columns"); columns_.push_back({key,Type::Null,row_>1}); }
      }
    }
    cells.assign(columns_.size(),Cell());
    for(size_t i=0;i<columns_.size();++i) { auto v=values.find(columns_[i].name); if(v!=values.end()) cells[i]=v->second; }
    return true;
  }
  ParquetReader parquet_; std::vector<size_t> projection_; bool projected_=false;
  Options options_; FILE *file_=nullptr; Stamp stamp_; bool eof_=false; uint64_t row_=0;
  std::string error_; std::vector<Column> columns_;
};
}}}
// Bound file descriptor metadata is carried in a constant plan expression. It is
// checked again at execution, so prepared plans cannot bypass a changed switch,
// secure_file_priv, or file identity.
namespace oceanbase { namespace sql { namespace file_sql {
static constexpr const char *PLAN_PREFIX = "seekdb-file-sql-v1:";
struct Bound {
  Options options; Stamp stamp; std::vector<Column> columns;
  bool discover = false, describe = false;
};
inline std::vector<Column> describe_columns() {
  return {{"column_name",Type::String,false},{"column_type",Type::String,false},{"null",Type::String,false},
          {"key",Type::String,true},{"default",Type::String,true},{"extra",Type::String,true}};
}
inline std::string pack(const Bound &b) {
  rapidjson::StringBuffer out; rapidjson::Writer<rapidjson::StringBuffer> w(out);
  w.StartObject();
  auto str=[&](const char *k,const std::string &v){w.Key(k);w.String(v.data(),static_cast<rapidjson::SizeType>(v.size()));};
  str("path",b.options.path);str("format",b.options.format);str("nullstr",b.options.nullstr);
  w.Key("delimiter");w.Int(static_cast<unsigned char>(b.options.delimiter));
  w.Key("quote");w.Int(static_cast<unsigned char>(b.options.quote));
  w.Key("escape");w.Int(static_cast<unsigned char>(b.options.escape));
  w.Key("header");w.Bool(b.options.header);w.Key("glob");w.Bool(b.discover);w.Key("describe");w.Bool(b.describe);
  w.Key("stamp");w.StartArray();w.Uint64(b.stamp.device);w.Uint64(b.stamp.inode);w.Uint64(b.stamp.size);w.Int64(b.stamp.mtime);w.Int64(b.stamp.nanos);w.EndArray();
  w.Key("columns");w.StartArray();for(auto &c:b.columns){w.StartArray();w.String(c.name.data(),static_cast<rapidjson::SizeType>(c.name.size()));w.Int(static_cast<int>(c.type));w.Bool(c.nullable);w.Int(c.scale);w.EndArray();}w.EndArray();w.EndObject();
  return std::string(PLAN_PREFIX)+std::string(out.GetString(),out.GetSize());
}
inline bool unpack(const std::string &text,Bound &b) {
  if(text.compare(0,strlen(PLAN_PREFIX),PLAN_PREFIX)!=0) return false;
  rapidjson::Document d;d.Parse(text.c_str()+strlen(PLAN_PREFIX));
  if(d.HasParseError() || !d.IsObject() || !d.HasMember("columns") || !d["columns"].IsArray())return false;
  b.options.path=d["path"].GetString();b.options.format=d["format"].GetString();b.options.nullstr=d["nullstr"].GetString();
  b.options.delimiter=static_cast<char>(d["delimiter"].GetInt());b.options.quote=static_cast<char>(d["quote"].GetInt());b.options.escape=static_cast<char>(d["escape"].GetInt());
  b.options.header=d["header"].GetBool();b.discover=d["glob"].GetBool();b.describe=d["describe"].GetBool();
  const auto &s=d["stamp"];b.stamp.device=s[0].GetUint64();b.stamp.inode=s[1].GetUint64();b.stamp.size=s[2].GetUint64();b.stamp.mtime=s[3].GetInt64();b.stamp.nanos=s[4].GetInt64();
  b.columns.clear();for(auto &c:d["columns"].GetArray())b.columns.push_back({c[0].GetString(),static_cast<Type>(c[1].GetInt()),c[2].GetBool(),c.Size()>3?c[3].GetInt():0});
  return true;
}
inline bool secure_path(const std::string &input,const std::string &root,std::string &canonical,std::string &error,bool pattern=false) {
  namespace fs=std::filesystem;std::error_code ec;
  if(input.empty() || input.find('\0')!=std::string::npos || root.empty() || root=="NULL") {error="secure_file_priv denies file access";return false;}
  fs::path allowed=fs::canonical(fs::u8path(root),ec);if(ec){error="invalid secure_file_priv";return false;}
  fs::path source=fs::u8path(input),candidate=fs::canonical(pattern?source.parent_path().empty()?fs::path("."):source.parent_path():source,ec);
  if(ec){error="file or parent directory does not exist";return false;}
  auto a=allowed.begin(),p=candidate.begin();for(;a!=allowed.end() && p!=candidate.end();++a,++p){
#ifdef _WIN32
    std::wstring x=a->wstring(),y=p->wstring();std::transform(x.begin(),x.end(),x.begin(),::towlower);std::transform(y.begin(),y.end(),y.begin(),::towlower);if(x!=y)break;
#else
    if(*a!=*p)break;
#endif
  }
  if(a!=allowed.end()){error="path is outside secure_file_priv";return false;}
  if(pattern)candidate/=source.filename();
  canonical=candidate.string();return true;
}
inline bool wildcard(const std::string &pattern,const std::string &name) {
  size_t i=0,j=0,star=std::string::npos,mark=0;
  while(j<name.size()) {
    if(i<pattern.size() && (pattern[i]=='?' || pattern[i]==name[j])){++i;++j;}
    else if(i<pattern.size() && pattern[i]=='*'){star=i++;mark=j;}
    else if(star!=std::string::npos){i=star+1;j=++mark;}
    else return false;
  }
  while(i<pattern.size() && pattern[i]=='*')++i;return i==pattern.size();
}
inline bool discover(const Options &options,const std::string &root,std::vector<std::string> &files,std::string &error) {
  namespace fs=std::filesystem;std::string path;
  if(!secure_path(options.path,root,path,error,true))return false;
  fs::path p=fs::u8path(path);std::error_code ec;
  fs::directory_iterator it(p.parent_path(),ec),end;
  if(ec){error="cannot enumerate directory";return false;}
  for(;it!=end;it.increment(ec)){
    if(ec){error="directory changed during discovery";return false;}
    if(!wildcard(p.filename().string(),it->path().filename().string()))continue;
    std::string resolved;if(!secure_path(it->path().string(),root,resolved,error))return false;
    if(fs::is_regular_file(fs::u8path(resolved),ec))files.push_back(fs::u8path(resolved).generic_string());
    if(ec || files.size()>10000){error="file discovery limit or filesystem error";return false;}
  }
  std::sort(files.begin(),files.end());return true;
}
}}}
namespace oceanbase { namespace sql { namespace file_sql {
class CopyWriter {
public:
  ~CopyWriter(){abort();}
  bool open(const Options &options,const std::string &root,std::string &error){
    options_=options;
    if(!secure_path(options.path,root,destination_,error,true))return false;
    std::error_code ec;
    if(std::filesystem::exists(std::filesystem::u8path(destination_),ec)||ec){error="COPY destination already exists or cannot be checked";return false;}
    static std::atomic<uint64_t> sequence{0};
    temporary_=destination_+".seekdb-copy-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(++sequence)+".tmp";
    file_=fopen(temporary_.c_str(),"wbx");
    if(!file_){error="cannot create COPY temporary file";return false;}return true;
  }
  bool row(const std::vector<Cell> &cells,std::string &error){
    if(!file_){error="COPY writer is not open";return false;}
    std::string line;
    for(size_t i=0;i<cells.size();++i){if(i)line+=options_.delimiter;const Cell &c=cells[i];
      if(c.null)line+=options_.nullstr;
      else{bool quote=c.text.empty()||c.text==options_.nullstr||c.text.find_first_of(std::string()+options_.delimiter+options_.quote+'\r'+'\n')!=std::string::npos;
        if(quote)line+=options_.quote;
        for(char ch:c.text){if(quote&&(ch==options_.quote||ch==options_.escape))line+=options_.escape;line+=ch;}
        if(quote)line+=options_.quote;}
      if(line.size()>64*1024*1024){error="COPY row exceeds 64 MiB limit";return false;}
    }
    line+='\n';if(bytes_>1024ULL*1024*1024-line.size()){error="COPY output exceeds 1 GiB limit";return false;}
    if(fwrite(line.data(),1,line.size(),file_)!=line.size()){error="COPY write failed";return false;}bytes_+=line.size();return true;
  }
  bool finish(std::string &error){
    if(!file_){error="COPY writer is not open";return false;}
    if(fflush(file_)){error="COPY flush failed";return false;}
#ifdef _WIN32
    if(_commit(_fileno(file_))){error="COPY sync failed";return false;}
#else
    if(fsync(fileno(file_))){error="COPY sync failed";return false;}
#endif
    if(fclose(file_)){file_=nullptr;error="COPY close failed";return false;}file_=nullptr;
    std::error_code ec;
#ifdef _WIN32
    if(!MoveFileExW(std::filesystem::u8path(temporary_).c_str(),std::filesystem::u8path(destination_).c_str(),0)) {error="COPY publish failed (destination may already exist)";return false;}
#else
    std::filesystem::create_hard_link(std::filesystem::u8path(temporary_),std::filesystem::u8path(destination_),ec);
    if(ec){error="COPY publish failed (destination may already exist)";return false;}
    std::filesystem::remove(std::filesystem::u8path(temporary_),ec);
#endif
    temporary_.clear();return true;
  }
  void abort(){if(file_){fclose(file_);file_=nullptr;}if(!temporary_.empty()){std::error_code ec;std::filesystem::remove(std::filesystem::u8path(temporary_),ec);temporary_.clear();}}
private:
  Options options_;FILE *file_=nullptr;std::string destination_,temporary_;uint64_t bytes_=0;
};
}}}
