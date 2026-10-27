// Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
#include "sql/engine/basic/file_scan_utils.h"
#include <cassert>
#include <fstream>
#include <iostream>
using namespace oceanbase::sql::file_sql;
namespace fs=std::filesystem;
static void write(const fs::path &p,const std::string &s){std::ofstream f(p,std::ios::binary);f<<s;}
int main(int argc,char **argv) {
  assert(argc==2);
  fs::path dir=fs::u8path(argv[1]);fs::create_directories(dir);
  Options o;o.path=(dir/"quoted.csv").string();
  write(o.path,"id,name,id,day\r\n1,\"hello,\r\nworld\",10,2024-02-29\r\n2,\"say \"\"hi\"\"\",11,\r\n");
  Reader r(o);assert(r.infer());auto schema=r.columns();auto stamp=r.stamp();
  assert(schema.size()==4 && schema[2].name=="id__2" && schema[0].type==Type::Int);
  assert(schema[3].type==Type::Date && schema[3].nullable);
  std::vector<Cell> row;assert(r.open(&stamp));assert(r.next(row));assert(row[1].text=="hello,\nworld");
  assert(r.next(row) && row[1].text=="say \"hi\"" && row[3].null);assert(!r.next(row) && !r.failed());
  assert(r.open(&stamp));assert(r.next(row));r.close();
  write(o.path,"id\n1\n");assert(!r.open(&stamp));
  write(o.path,"x,y\n\"bad\n");Reader bad(o);assert(!bad.infer());
  write(o.path,"x,y\n1,2,3\n");Reader width(o);assert(!width.infer());
  o.path=(dir/"escape.csv").string();o.escape=92;
  write(o.path,"x\n\"C:\\folder, said \\\"hi\\\"\"\n");Reader escaped(o);assert(escaped.infer()&&escaped.open());
  assert(escaped.next(row)&&row[0].text=="C:\\folder, said \"hi\"");o.escape=34;
  o.format="json";o.path=(dir/"union.jsonl").string();
  write(o.path,"{\"a\":1}\n{\"b\":true,\"a\":2.5}\n{\"a\":null}\n");Reader j(o);assert(j.infer());
  assert(j.columns().size()==2 && j.columns()[0].type==Type::Double && j.columns()[1].nullable);
  assert(j.open());assert(j.next(row) && row[1].null);assert(j.next(row) && row[1].text=="true");
  write(o.path,"{\"a\":[1]}\n");Reader nested(o);assert(!nested.infer());
  int64_t n;bool dt;
  assert(int_value("9223372036854775807",n) && n==INT64_MAX);
  assert(!int_value("9223372036854775808",n));
  assert(detect({"9223372036854775808",false})==Type::String);
  assert(!temporal("2023-02-29",dt,n));assert(temporal("1970-01-01",dt,n) && n==0);
  assert(temporal("2000-02-29T01:02:03.123456",dt,n));
  assert(widen(Type::Date,Type::DateTime)==Type::DateTime);
  std::string resolved,error;assert(secure_path(o.path,dir.string(),resolved,error));
  assert(!secure_path(o.path,(dir/"not-the-root").string(),resolved,error));
  assert(!secure_path(o.path,"",resolved,error));
  Options timestamps;timestamps.path=(dir/"timestamps.csv").string();
  write(timestamps.path,"t\n2026-10-27 01:02:03\n2026-10-27 01:02:03.123456\n");
  Reader time_reader(timestamps);assert(time_reader.infer());
  assert(time_reader.columns()[0].type==Type::DateTime && time_reader.columns()[0].scale==6);
  Bound time_bound;time_bound.options=timestamps;time_bound.columns=time_reader.columns();
  Bound time_restored;assert(unpack(pack(time_bound),time_restored) && time_restored.columns[0].scale==6);
  Bound b;b.options=o;b.columns=j.columns();b.stamp=j.stamp();Bound restored;
  assert(unpack(pack(b),restored));assert(restored.columns.size()==2 && restored.stamp==b.stamp);
  Options po;po.path=(dir/"simple.parquet").string();po.format="parquet";
  Reader parquet(po);if(!parquet.infer()){std::cerr<<parquet.error()<<"\n";return 2;}assert(parquet.columns().size()==1);
  assert(parquet.open());int count=0;int64_t sum=0;
  while(parquet.next(row)){int64_t value;assert(int_value(row[0].text,value));sum+=value;++count;}
  if(parquet.failed()){std::cerr<<parquet.error()<<"\n";return 3;}assert(count==7 && sum==21);
  for(const char *name:{"nullable-1.0.parquet","nullable-2.0.parquet"}){
    Options options;options.path=(dir/name).string();options.format="parquet";
    Reader reader(options);assert(reader.infer());assert(reader.columns().size()==7);
    assert(reader.columns()[4].type==Type::Date && reader.columns()[5].type==Type::DateTime);
    assert(reader.open());size_t rows=0;
    while(reader.next(row)){
      assert(row[6].null);
      if(rows==0){assert(row[0].text=="1"&&row[1].text=="true"&&row[2].text=="0.25");assert(row[4].text=="2024-02-29"&&row[5].text=="2026-10-27 01:02:03.123456");}
      if(rows==1)assert(row[1].null&&row[3].null&&row[4].null);
      if(rows==2)assert(row[0].null&&row[3].text=="中文");
      if(rows==3)assert(row[4].text=="1969-12-31"&&row[5].text=="1969-12-31 23:59:59.000000");
      ++rows;
    }
    if(reader.failed()){std::cerr<<name<<": "<<reader.error()<<"\n";return 4;}assert(rows==5);
    Reader projected(options);assert(projected.infer());projected.projection({0});assert(projected.open());rows=0;
    while(projected.next(row)){assert(row[3].null);++rows;}assert(!projected.failed()&&rows==5);
  }
  std::string snappy_out;assert(parquet_detail::snappy(std::string("\x05\x10hello",7),snappy_out) && snappy_out=="hello");
  assert(!parquet_detail::snappy(std::string("\x05\x01\x00",3),snappy_out));
  Options export_options;export_options.path=(dir/"roundtrip.csv").string();export_options.nullstr="NULL";
  std::error_code cleanup_error;fs::remove(export_options.path,cleanup_error);
  CopyWriter writer;assert(writer.open(export_options,dir.string(),error));
  assert(writer.row({{"a",false},{"b",false},{"c",false}},error));
  assert(writer.row({{"hello,\nworld",false},{"NULL",false},{"",true}},error));
  assert(writer.row({{"say \"hi\"",false},{"",false},{"Unicode: 中文",false}},error));
  assert(writer.finish(error));CopyWriter exists;assert(!exists.open(export_options,dir.string(),error));
  Reader roundtrip(export_options);assert(roundtrip.infer());assert(roundtrip.open());
  assert(roundtrip.next(row) && row[0].text=="hello,\nworld" && row[1].text=="NULL" && !row[1].null && row[2].null);
  assert(roundtrip.next(row) && row[0].text=="say \"hi\"" && row[1].text.empty() && !row[1].null);
  Options abandoned=export_options;abandoned.path=(dir/"abandoned.csv").string();fs::remove(abandoned.path,cleanup_error);
  {CopyWriter abort;assert(abort.open(abandoned,dir.string(),error));assert(abort.row({{"temporary",false}},error));}
  assert(!fs::exists(abandoned.path));
  std::cout<<"File scan regression checks passed\n";
}
