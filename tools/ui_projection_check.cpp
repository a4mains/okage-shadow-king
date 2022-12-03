// Standalone research fixture generator/comparator. Never a game input path.
#include "okage/ui_projection.hpp"
#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <vector>
using U=std::uint32_t;
namespace {
using namespace okage;
constexpr unsigned input_words=108,output_words=112;
void require(bool b,const char* why){if(!b)throw std::runtime_error(why);}
std::vector<U> read(const char* path){std::ifstream f(path,std::ios::binary|std::ios::ate);require(bool(f),"Cannot read fixture");
    const auto n=f.tellg();require(n>=0&&n<=1024*1024&&n%4==0,"Fixture byte extent");std::vector<U> data(static_cast<std::size_t>(n)/4);f.seekg(0);require(bool(f.read(reinterpret_cast<char*>(data.data()),n)),"Short fixture");return data;}
void fresh(const char* path){require(!std::filesystem::exists(path)&&!std::filesystem::is_symlink(path),"Output must be fresh");}
void write(const char* path,const void* p,std::size_t size){fresh(path);std::ofstream f(path,std::ios::binary);require(bool(f.write(static_cast<const char*>(p),size)),"Cannot write output");}
Matrix4 identity(){return {{{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}}};}
UiProjectionParameters setup(){return {{{1,1,1,0},{0,0,0,1},{1,1,1,0},{0,0,0,0},
    {2048,2048,64,0},{320,112,32,128},{1728,1936,-1,0},{2368,2160,1,1}}};}
std::vector<U> fixtures(){
    std::vector<U> data;
    for(unsigned kind=0;kind<5;++kind)for(unsigned textured=0;textured<2;++textured)for(unsigned first=0;first<2;++first){
        std::array<U,input_words> row{};row[0]=unsigned(data.size()/input_words);row[1]=first;row[2]=textured;
        auto p=setup();auto matrix=identity();
        std::array<Float4,4> positions={Float4{-.5f,-.5f,0,1},Float4{.5f,-.5f,0,1},Float4{.5f,.5f,0,1},Float4{-.5f,.5f,0,1}};
        std::array<Float4,4> colors,aux;std::array<std::array<float,2>,4> uv;
        for(unsigned i=0;i<4;++i){colors[i]={1,.25f,2,.5f};aux[i]={128,128,128,128};uv[i]={float(i+1)*.125f,float(i+2)*.125f};}
        if(kind==1){p[0][2]=2;for(auto& v:positions)v[2]=.25f;} // Branch-delay column2 discriminator.
        if(kind==2){p[0][0]=.3f;matrix[0][0]=.3f;matrix[1][0]=.3f;matrix[1][1]=0;
            for(auto& v:positions)v={.3f,1.1f,0,1};}
        if(kind==3){for(unsigned i=0;i<4;++i){uv[i]={-0.f,+0.f};colors[i][0]=-0.f;positions[i][2]=-0.f;}}
        if(kind==4){p[4][0]=2048.03125f;p[4][2]=64.03125f;p[6][3]=.25f;p[7][3]=2;
            for(auto& c:colors)c={.50390625f,1.9921875f,3.f,.0078125f};}
        std::memcpy(row.data()+4,p.data(),128);std::memcpy(row.data()+36,matrix.data(),64);
        std::memcpy(row.data()+52,positions.data(),64);std::memcpy(row.data()+68,colors.data(),64);
        std::memcpy(row.data()+84,aux.data(),64);std::memcpy(row.data()+100,uv.data(),32);
        data.insert(data.end(),row.begin(),row.end());
    }
    return data;
}
int compare(const char* input_path,const char* original_path,const char* output){
    fresh(output);const auto input=read(input_path),original=read(original_path);
    require(!input.empty()&&input.size()%input_words==0&&input.size()/input_words<=64,"Input row extent");
    const auto rows=input.size()/input_words;require(original.size()==rows*output_words,"Original output extent");
    std::set<U> ids;std::uint64_t checked=0,differences=0;std::string first_difference;
    auto check=[&](unsigned row,const std::string& channel,unsigned lane,U expected,U actual){
        ++checked;if(expected==actual)return;++differences;
        if(first_difference.empty())first_difference="{\"row\":"+std::to_string(row)+",\"channel\":\""+channel+"\",\"lane\":"+std::to_string(lane)+",\"native\":"+std::to_string(expected)+",\"original\":"+std::to_string(actual)+"}";
    };
    for(unsigned row=0;row<rows;++row){
        const auto* f=input.data()+row*input_words;const auto* r=original.data()+row*output_words;
        require(f[1]<=1&&f[2]<=1&&!f[3]&&ids.insert(f[0]).second,"Input header or duplicate ID");
        for(unsigned i=4;i<input_words;++i)require((f[i]&0x7f800000)!=0x7f800000,"Nonfinite input");
        UiProjectionParameters p;Matrix4 matrix;std::array<UiProjectionVertex,4> vertices;
        std::memcpy(p.data(),f+4,128);std::memcpy(matrix.data(),f+36,64);
        for(unsigned i=0;i<4;++i){std::memcpy(vertices[i].position.data(),f+52+i*4,16);std::memcpy(vertices[i].source_color.data(),f+68+i*4,16);
            std::memcpy(vertices[i].auxiliary_color.data(),f+84+i*4,16);std::memcpy(vertices[i].uv.data(),f+100+i*2,8);}
        const auto native=ui_project_quad(p,matrix,vertices,f[2]!=0);require(native.emitted,"Native synthetic quad unexpectedly rejected");
        const std::array<U,8> header={f[0],f[1],f[2],0,0,0,13,0x2c50};
        for(unsigned i=0;i<8;++i)check(row,"metadata",i,header[i],r[i]);
        for(unsigned i=0;i<4;++i){const auto& n=native.vertices[i];
            for(unsigned j=0;j<4;++j){check(row,"clip",i*4+j,__builtin_bit_cast(U, (n.clip[j])),r[8+i*4+j]);check(row,"screen",i*4+j,__builtin_bit_cast(U, (n.screen[j])),r[24+i*4+j]);}
            check(row,"clip_flags",i,n.clip_flags,r[40+i]);
            for(unsigned j=0;j<4;++j){const U expected=j<2?f[100+i*2+j]:j==2?0x3f800000u:n.clip_flags;check(row,"direct_uv_cache",i*4+j,expected,r[44+i*4+j]);}
            const auto* packet=r+60+(1+i*3)*4;
            if(f[2])for(unsigned j=0;j<3;++j)check(row,"stq",i*3+j,__builtin_bit_cast(U, (n.stq[j])),packet[j]);
            for(unsigned j=0;j<4;++j){check(row,"rgba",i*4+j,n.rgba[j],packet[4+j]);check(row,"xyzf",i*4+j,n.xyzf[j],packet[8+j]);}
        }
    }
    const std::string report="{\n  \"schema\":\"okage-ui-projection-exact-v1\",\n  \"rows\":"+std::to_string(rows)+",\n  \"checked_words\":"+std::to_string(checked)+",\n  \"differing_words\":"+std::to_string(differences)+",\n  \"equal\":"+(differences?"false":"true")+",\n  \"first_difference\":"+(first_difference.empty()?"null":first_difference)+",\n  \"scope\":\"Exact native versus original VU clip/screen/flags/UV and emitted STQ/RGBA/XYZF; GIF tag and unused STQ lanes excluded. Unclipped fixtures, independent reset aggregates. No EE compiler, raster, or full UI claim.\"\n}\n";
    write(output,report.data(),report.size());std::cout<<report;return differences?1:0;
}
}
int main(int argc,char** argv){try{
    static_assert(std::endian::native==std::endian::little);
    if(argc==3&&std::string(argv[1])=="generate"){const auto f=fixtures();write(argv[2],f.data(),f.size()*4);std::cout<<f.size()/input_words<<" synthetic UI projection rows\n";return 0;}
    if(argc==5&&std::string(argv[1])=="compare")return compare(argv[2],argv[3],argv[4]);
    throw std::runtime_error("Usage: ui-projection-check generate FRESH_INPUT | compare INPUT ORIGINAL FRESH_REPORT");
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}}
