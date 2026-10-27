// Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
#pragma once
// Parquet's compact-Thrift footer and bounded row-group decoder. This component
// decodes a file format only; predicates, joins and aggregates belong to SQL.
namespace oceanbase { namespace sql { namespace file_sql {
namespace parquet_detail {
struct Node { int64_t number=0; std::string text; std::vector<Node> list; std::map<int,Node> fields; };
class Compact {
public:
  Compact(const std::string &bytes):data_(bytes){}
  bool object(Node &out,int depth=0) {
    if(depth>32)return false;int id=0;
    while(pos_<data_.size()) { unsigned char h=data_[pos_++];if(!h)return true;
      int type=h&15;if(h>>4)id+=h>>4;else{uint64_t v;if(!var(v))return false;id=zig(v);}
      if(id<=0 || out.fields.count(id))return false;Node n;if(!value(type,n,depth+1))return false;out.fields.emplace(id,std::move(n));
    }return false;
  }
  size_t position() const{return pos_;}
private:
  static int64_t zig(uint64_t v){return static_cast<int64_t>(v>>1)^-static_cast<int64_t>(v&1);}
  bool var(uint64_t &v){v=0;for(unsigned i=0;i<10 && pos_<data_.size();++i){unsigned char b=data_[pos_++];if(i==9 && b>1)return false;v|=uint64_t(b&127)<<(i*7);if(!(b&128))return true;}return false;}
  bool value(int t,Node &n,int depth){
    if(depth>32)return false;uint64_t v=0;
    switch(t){
      case 1:n.number=1;return true;case 2:n.number=0;return true;
      case 3:if(pos_>=data_.size())return false;n.number=static_cast<int8_t>(data_[pos_++]);return true;
      case 4:case 5:case 6:if(!var(v))return false;n.number=zig(v);return true;
      case 7:if(data_.size()-pos_<8)return false;pos_+=8;return true;
      case 8:if(!var(v)||v>data_.size()-pos_)return false;n.text.assign(data_,pos_,static_cast<size_t>(v));pos_+=v;return true;
      case 9:case 10:{if(pos_>=data_.size())return false;unsigned char h=data_[pos_++];uint64_t count=h>>4;if(count==15&&!var(count))return false;if(count>1000000)return false;for(uint64_t i=0;i<count;++i){Node child;if(!value(h&15,child,depth+1))return false;n.list.push_back(std::move(child));}return true;}
      case 11:{if(!var(v)||v>1000000)return false;if(!v)return true;if(pos_>=data_.size())return false;unsigned char h=data_[pos_++];for(uint64_t i=0;i<v;++i){Node a,b;if(!value(h>>4,a,depth+1)||!value(h&15,b,depth+1))return false;}return true;}
      case 12:return object(n,depth+1);default:return false;
    }
  }
  const std::string &data_;size_t pos_=0;
};
inline const Node &field(const Node &n,int id){static const Node empty;auto it=n.fields.find(id);return it==n.fields.end()?empty:it->second;}
inline bool var(const std::string &s,size_t &p,uint64_t &v){v=0;for(unsigned i=0;i<10 && p<s.size();++i){unsigned char b=s[p++];if(i==9 && b>1)return false;v|=uint64_t(b&127)<<(i*7);if(!(b&128))return true;}return false;}
inline uint64_t little(const char *p,int bytes){uint64_t v=0;for(int i=0;i<bytes;++i)v|=uint64_t(static_cast<unsigned char>(p[i]))<<(8*i);return v;}
inline bool seek(FILE *f,uint64_t offset){
  if(offset>static_cast<uint64_t>(INT64_MAX))return false;
#ifdef _WIN32
  return _fseeki64(f,static_cast<int64_t>(offset),SEEK_SET)==0;
#else
  return fseeko(f,static_cast<off_t>(offset),SEEK_SET)==0;
#endif
}
inline bool snappy(const std::string &in,std::string &out){
  size_t p=0;uint64_t size;if(!var(in,p,size)||size>64*1024*1024)return false;out.clear();out.reserve(static_cast<size_t>(size));
  while(p<in.size() && out.size()<size){unsigned char tag=in[p++];size_t n=0,offset=0;
    if((tag&3)==0){n=(tag>>2)+1;if(n>=61){int bytes=static_cast<int>(n)-60;if(bytes>4||in.size()-p<static_cast<size_t>(bytes))return false;n=static_cast<size_t>(little(in.data()+p,bytes))+1;p+=bytes;}if(n>in.size()-p||n>size-out.size())return false;out.append(in,p,n);p+=n;}
    else{if((tag&3)==1){if(p>=in.size())return false;n=4+((tag>>2)&7);offset=((tag&0xe0)<<3)|static_cast<unsigned char>(in[p++]);}
      else{int bytes=(tag&3)==2?2:4;if(in.size()-p<static_cast<size_t>(bytes))return false;n=1+(tag>>2);offset=static_cast<size_t>(little(in.data()+p,bytes));p+=bytes;}
      if(!offset||offset>out.size()||n>size-out.size())return false;for(size_t i=0;i<n;++i)out.push_back(out[out.size()-offset]);}
  }return out.size()==size && p==in.size();
}
inline bool hybrid(const std::string &bytes,size_t &p,int bits,size_t count,std::vector<uint32_t> &out){
  if(bits<0||bits>32)return false;out.clear();out.reserve(count);
  while(out.size()<count){uint64_t run;if(!var(bytes,p,run)||!run)return false;
    if(!(run&1)){size_t n=static_cast<size_t>(run>>1);int width=(bits+7)/8;if(bytes.size()-p<static_cast<size_t>(width)||n>count-out.size())return false;uint32_t v=static_cast<uint32_t>(little(bytes.data()+p,width));p+=width;out.insert(out.end(),n,v);}
    else{uint64_t groups=run>>1;if(groups>10000000)return false;size_t n=static_cast<size_t>(groups*8),size=static_cast<size_t>(groups*bits);if(size>bytes.size()-p)return false;
      for(size_t i=0;i<n && out.size()<count;++i){uint32_t v=0;for(int b=0;b<bits;++b){size_t bit=i*bits+b;v|=uint32_t((static_cast<unsigned char>(bytes[p+bit/8])>>(bit%8))&1)<<b;}out.push_back(v);}p+=size;}
  }return true;
}
inline std::string date_text(int64_t day){
  day+=719468;int64_t era=(day>=0?day:day-146096)/146097;unsigned doe=static_cast<unsigned>(day-era*146097),yo=(doe-doe/1460+doe/36524-doe/146096)/365;int64_t y=yo+era*400;unsigned doy=doe-(365*yo+yo/4-yo/100),mp=(5*doy+2)/153,d=doy-(153*mp+2)/5+1,m=mp<10?mp+3:mp-9;y+=m<=2;char s[40];snprintf(s,sizeof(s),"%04lld-%02u-%02u",static_cast<long long>(y),m,d);return s;
}
}
class ParquetReader {
  struct Description{int physical=0,converted=-1,width=0;bool optional=false;};
public:
  bool metadata(FILE *file,uint64_t size,std::vector<Column> &columns,std::string &error){
    file_=file;size_=size;metadata_=parquet_detail::Node();columns.clear();descriptions_.clear();group_=0;position_=0;rows_=0;error_.clear();
    if(size<12)return fail(error,"truncated Parquet file");
    char magic[4],tail[8];if(!parquet_detail::seek(file,0)||fread(magic,1,4,file)!=4||memcmp(magic,"PAR1",4)||!parquet_detail::seek(file,size-8)||fread(tail,1,8,file)!=8||memcmp(tail+4,"PAR1",4))return fail(error,"invalid Parquet magic");
    uint64_t len=parquet_detail::little(tail,4);if(len>16*1024*1024||len>size-12)return fail(error,"Parquet footer exceeds limit");
    std::string footer(static_cast<size_t>(len),'\0');if(!parquet_detail::seek(file,size-8-len)||fread(footer.data(),1,footer.size(),file)!=footer.size())return fail(error,"cannot read footer");
    parquet_detail::Compact parser(footer);if(!parser.object(metadata_))return fail(error,"invalid Parquet footer");
    const auto &schema=parquet_detail::field(metadata_,2).list;
    if(schema.empty()||schema.size()>1025||parquet_detail::field(schema[0],5).number!=static_cast<int64_t>(schema.size()-1))return fail(error,"only flat Parquet schemas are supported");
    for(size_t i=1;i<schema.size();++i){const auto &s=schema[i];Description d;d.physical=static_cast<int>(parquet_detail::field(s,1).number);d.width=static_cast<int>(parquet_detail::field(s,2).number);d.optional=parquet_detail::field(s,3).number==1;
      if(parquet_detail::field(s,3).number==2||parquet_detail::field(s,5).number)return fail(error,"nested or repeated Parquet fields are unsupported");
      auto converted=s.fields.find(6);if(converted!=s.fields.end())d.converted=static_cast<int>(converted->second.number);
      Type type=Type::String;if(d.physical==0)type=Type::Bool;else if(d.physical==1||d.physical==2)type=Type::Int;else if(d.physical==4||d.physical==5)type=Type::Double;
      if(d.converted==6)type=Type::Date;else if(d.converted==9||d.converted==10)type=Type::DateTime;
      if(d.physical==3||d.physical>7||d.physical<0)return fail(error,"unsupported Parquet physical type");
      columns.push_back({parquet_detail::field(s,4).text,type,d.optional,type==Type::DateTime?(d.converted==9?3:6):0});descriptions_.push_back(d);
    }
    return true;
  }
  void projection(const std::vector<size_t> &p){projection_=p;projected_=true;}
  bool next(std::vector<Cell> &out,std::string &error){
    const auto &groups=parquet_detail::field(metadata_,4).list;
    while(position_>=rows_){if(group_>=groups.size())return false;const auto &g=groups[group_++];rows_=static_cast<size_t>(parquet_detail::field(g,3).number);position_=0;
      if(rows_>1000000)return fail(error,"Parquet row group exceeds one million rows");
      const auto &chunks=parquet_detail::field(g,1).list;if(chunks.size()!=descriptions_.size())return fail(error,"Parquet chunk/schema mismatch");
      values_.clear();values_.resize(chunks.size());size_t total=0;
      for(size_t i=0;i<chunks.size();++i){if(projected_ && std::find(projection_.begin(),projection_.end(),i)==projection_.end())continue;
        if(!decode(chunks[i],descriptions_[i],rows_,values_[i],error))return false;
        for(auto &v:values_[i]){total+=sizeof(Cell)+v.text.size();if(total>64*1024*1024)return fail(error,"Parquet row group exceeds 64 MiB limit");}}
    }
    out.assign(descriptions_.size(),Cell());for(size_t i=0;i<values_.size();++i)if(!values_[i].empty())out[i]=values_[i][position_];++position_;return true;
  }
  bool failed() const{return !error_.empty();}
private:
  bool fail(std::string &error,const char *message){error_=message;error=message;return false;}
  bool plain(const std::string &bytes,size_t &p,const Description &d,size_t count,std::vector<Cell> &out){
    out.clear();if(count>1000000)return false;out.reserve(count);
    size_t memory=0;for(size_t i=0;i<count;++i){Cell cell;cell.null=false;int width=d.physical==1||d.physical==4?4:d.physical==2||d.physical==5?8:d.physical==7?d.width:0;
      if(d.physical==0){if(p+i/8>=bytes.size())return false;cell.text=((static_cast<unsigned char>(bytes[p+i/8])>>(i%8))&1)?"true":"false";}
      else if(d.physical==6){if(bytes.size()-p<4)return false;uint64_t len=parquet_detail::little(bytes.data()+p,4);p+=4;if(len>bytes.size()-p||len>8*1024*1024)return false;cell.text.assign(bytes,p,static_cast<size_t>(len));p+=len;}
      else{if(width<=0||static_cast<size_t>(width)>bytes.size()-p)return false;
        if(d.physical==4){uint32_t bits=static_cast<uint32_t>(parquet_detail::little(bytes.data()+p,4));float f;memcpy(&f,&bits,4);if(!std::isfinite(f))return false;char s[64];snprintf(s,sizeof(s),"%.9g",f);cell.text=s;}
        else if(d.physical==5){uint64_t bits=parquet_detail::little(bytes.data()+p,8);double f;memcpy(&f,&bits,8);if(!std::isfinite(f))return false;char s[64];snprintf(s,sizeof(s),"%.17g",f);cell.text=s;}
        else if(d.physical==7)cell.text.assign(bytes,p,width);
        else{int64_t n=d.physical==1?static_cast<int32_t>(parquet_detail::little(bytes.data()+p,4)):static_cast<int64_t>(parquet_detail::little(bytes.data()+p,8));
          if(d.converted==6)cell.text=parquet_detail::date_text(n);
          else if(d.converted==9||d.converted==10){if(d.converted==9){if(n>INT64_MAX/1000||n<INT64_MIN/1000)return false;n*=1000;}int64_t day=n/86400000000LL,us=n%86400000000LL;if(us<0){--day;us+=86400000000LL;}char t[40];snprintf(t,sizeof(t)," %02lld:%02lld:%02lld.%06lld",static_cast<long long>(us/3600000000LL),static_cast<long long>(us/60000000LL%60),static_cast<long long>(us/1000000%60),static_cast<long long>(us%1000000));cell.text=parquet_detail::date_text(day)+t;}
          else cell.text=std::to_string(n);}
        p+=width;
      }memory+=sizeof(Cell)+cell.text.size();if(memory>64*1024*1024)return false;out.push_back(std::move(cell));
    }
    if(d.physical==0)p+=(count+7)/8;return true;
  }
  bool decode(const parquet_detail::Node &chunk,const Description &d,size_t rows,std::vector<Cell> &out,std::string &error){
    using namespace parquet_detail;
    const auto &meta=field(chunk,3);int codec=static_cast<int>(field(meta,4).number);if(codec!=0&&codec!=1)return fail(error,"unsupported Parquet codec (supported: uncompressed, Snappy)");
    int64_t start=field(meta,9).number;auto dict_offset=meta.fields.find(11);if(dict_offset!=meta.fields.end() && dict_offset->second.number>=0)start=std::min(start,dict_offset->second.number);
    int64_t compressed=field(meta,7).number;if(start<4||compressed<0||static_cast<uint64_t>(start)>size_||static_cast<uint64_t>(compressed)>size_-start||compressed>64*1024*1024)return fail(error,"invalid Parquet column bounds");
    std::string chunk_bytes(static_cast<size_t>(compressed),'\0');if(!seek(file_,start)||fread(chunk_bytes.data(),1,chunk_bytes.size(),file_)!=chunk_bytes.size())return fail(error,"cannot read Parquet column");
    size_t p=0,output_bytes=0;std::vector<Cell> dictionary;
    while(out.size()<rows && p<chunk_bytes.size()){
      std::string remaining=chunk_bytes.substr(p);Compact parser(remaining);Node page;if(!parser.object(page))return fail(error,"invalid Parquet page header");p+=parser.position();
      int kind=static_cast<int>(field(page,1).number);int64_t len=field(page,3).number,raw_len=field(page,2).number;
      if(len<0||raw_len<0||raw_len>64*1024*1024||static_cast<uint64_t>(len)>chunk_bytes.size()-p)return fail(error,"invalid Parquet page bounds");
      std::string raw=chunk_bytes.substr(p,static_cast<size_t>(len));p+=len;
      const Node &header=field(page,kind==2?7:kind==3?8:5);size_t count=static_cast<size_t>(field(header,1).number),offset=0;
      if(count>1000000)return fail(error,"Parquet page value limit exceeded");
      std::string data;
      size_t levels=kind==3?static_cast<size_t>(field(header,5).number+field(header,6).number):0;
      if(levels>raw.size())return fail(error,"invalid Parquet level bounds");
      bool compressed_page=codec==1;
      if(kind==3){auto flag=header.fields.find(7);if(flag!=header.fields.end())compressed_page=compressed_page&&flag->second.number!=0;}
      if(compressed_page){std::string decoded;if(!snappy(raw.substr(levels),decoded))return fail(error,"invalid Snappy page");data=raw.substr(0,levels)+decoded;}else data=std::move(raw);
      if(data.size()!=static_cast<size_t>(raw_len))return fail(error,"Parquet decompressed size mismatch");
      if(kind==2){if(field(header,2).number!=0||!plain(data,offset,d,count,dictionary))return fail(error,"invalid Parquet dictionary");continue;}
      if(kind!=0 && kind!=3)continue;
      if(count>rows-out.size())return fail(error,"Parquet value count exceeds row group");
      std::vector<uint32_t> definitions(count,1);
      if(d.optional){std::string encoded;
        if(kind==3){size_t rep=static_cast<size_t>(field(header,6).number),def=static_cast<size_t>(field(header,5).number);if(rep || rep+def>data.size())return fail(error,"invalid Parquet levels");encoded=data.substr(rep,def);offset=rep+def;}
        else{if(data.size()<4)return fail(error,"missing Parquet definitions");size_t def=static_cast<size_t>(little(data.data(),4));if(def>data.size()-4)return fail(error,"invalid Parquet definitions");encoded=data.substr(4,def);offset=4+def;}
        size_t lp=0;if(!hybrid(encoded,lp,1,count,definitions))return fail(error,"invalid Parquet definition RLE");
      }else if(kind==3)offset=levels;
      size_t valid=0;for(auto x:definitions){if(x>1)return fail(error,"invalid definition level");valid+=x;}
      int encoding=static_cast<int>(field(header,kind==3?4:2).number);std::vector<Cell> decoded;size_t decoded_bytes=0;
      if(encoding==0){if(!plain(data,offset,d,valid,decoded))return fail(error,"invalid Parquet plain values");}
      else if(encoding==2||encoding==8){if(offset>=data.size())return fail(error,"missing dictionary bit width");int bits=static_cast<unsigned char>(data[offset++]);std::vector<uint32_t> ids;
        if(!hybrid(data,offset,bits,valid,ids))return fail(error,"invalid Parquet dictionary indices");for(auto id:ids){if(id>=dictionary.size())return fail(error,"Parquet dictionary index out of range");decoded_bytes+=sizeof(Cell)+dictionary[id].text.size();if(decoded_bytes>64*1024*1024)return fail(error,"Parquet dictionary expansion exceeds memory limit");decoded.push_back(dictionary[id]);}}
      else if(encoding==3 && d.physical==0){
        if(valid){
          if(data.size()-offset<4)return fail(error,"missing Boolean RLE length");
          size_t length=static_cast<size_t>(little(data.data()+offset,4));offset+=4;
          if(length>data.size()-offset)return fail(error,"invalid Boolean RLE length");
          std::string encoded=data.substr(offset,length);size_t rp=0;std::vector<uint32_t> bits;
          if(!hybrid(encoded,rp,1,valid,bits))return fail(error,"invalid Boolean RLE values");
          for(auto bit:bits){if(bit>1)return fail(error,"invalid Boolean value");decoded.push_back({bit?"true":"false",false});}
        }
      }
      else return fail(error,"unsupported Parquet data encoding");
      size_t index=0;for(auto level:definitions){
        output_bytes+=sizeof(Cell)+(level?decoded[index].text.size():0);
        if(output_bytes>64*1024*1024)return fail(error,"Parquet column exceeds memory limit");
        if(level)out.push_back(std::move(decoded[index++]));else out.emplace_back();
      }
    }
    return out.size()==rows || fail(error,"incomplete Parquet row group");
  }
  FILE *file_=nullptr;uint64_t size_=0;parquet_detail::Node metadata_;std::vector<Description> descriptions_;
  std::vector<std::vector<Cell>> values_;std::vector<size_t> projection_;bool projected_=false;
  size_t group_=0,position_=0,rows_=0;std::string error_;
};
}}}
