#include "okage/texture_address_batch.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <vector>

using Query = okage::TextureAddressQuery;
using Result = okage::TextureAddressResult;
using Clock = std::chrono::steady_clock;
namespace {
double seconds(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double>(b-a).count();
}
std::vector<unsigned char> read(const char* path, std::size_t bound) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f || f.tellg()<0 || static_cast<std::uint64_t>(f.tellg())>bound)
        throw std::runtime_error("missing/oversized file");
    std::vector<unsigned char> bytes(static_cast<std::size_t>(f.tellg()));
    f.seekg(0);
    if (!bytes.empty() && !f.read(reinterpret_cast<char*>(bytes.data()), bytes.size()))
        throw std::runtime_error("short file");
    return bytes;
}
std::uint32_t word(const unsigned char* p) {
    return std::uint32_t(p[0]) | std::uint32_t(p[1])<<8 | std::uint32_t(p[2])<<16 | std::uint32_t(p[3])<<24;
}
std::vector<Query> corpus(const char* path) {
    const auto bytes=read(path,16+1000000*20);
    if(bytes.size()<16 || word(bytes.data())!=0x52444147 || word(bytes.data()+4)!=1 || word(bytes.data()+12)!=5)
        throw std::runtime_error("bad GADR header");
    const auto count=word(bytes.data()+8);
    if(count!=356160 || bytes.size()!=16+std::size_t(count)*20) throw std::runtime_error("wrong corpus extent");
    std::vector<Query> out(count);
    for(std::size_t i=0;i<count;++i) {
        const auto* p=bytes.data()+16+i*20;
        out[i]={word(p),word(p+4),word(p+8),word(p+12),word(p+16)};
    }
    return out;
}
std::vector<Result> reference(const char* path, std::size_t count) {
    const auto bytes=read(path,count*4);
    if(bytes.size()!=count*4) throw std::runtime_error("wrong reference extent");
    std::vector<Result> out(count);
    for(std::size_t i=0;i<count;++i) out[i]={word(bytes.data()+i*4),1};
    return out;
}
void check(std::span<const Result> actual, std::span<const Result> expected, const char* label) {
    if(actual.size()!=expected.size()) throw std::runtime_error("result count mismatch");
    for(std::size_t i=0;i<actual.size();++i) if(actual[i]!=expected[i]) {
        std::cerr<<label<<" mismatch row="<<i<<" actual="<<actual[i].address<<","<<actual[i].valid
                 <<" expected="<<expected[i].address<<","<<expected[i].valid<<'\n';
        throw std::runtime_error("exact comparison failed");
    }
}
void json_number(double value) { if(std::isfinite(value)) std::cout<<value; else std::cout<<"null"; }
struct Workload { std::string name; std::vector<Query> input; };
std::vector<Workload> workloads(const std::vector<Query>& retained) {
    std::vector<Workload> out;
    for(auto n:{0u,1u,31u,33u}) {
        Workload w{"mixed-"+std::to_string(n),{}};
        for(unsigned i=0;i<n;++i) w.input.push_back({i%2?19u:0u,16320,3,i%64,i/64});
        out.push_back(std::move(w));
    }
    for(auto n:{4096u,16384u,143360u,1048576u}) for(auto psm:{0u,19u}) {
        unsigned width=n==4096?64:n==16384?128:n==143360?640:1024;
        Workload w{"synthetic-psm"+std::to_string(psm)+"-"+std::to_string(n),{}};
        w.input.reserve(n);
        for(unsigned i=0;i<n;++i) w.input.push_back({psm,16320,width/64,i%width,i/width});
        out.push_back(std::move(w));
    }
    out.push_back({"retained-corpus-356160",retained});
    return out;
}
void dump_inputs(const std::vector<Workload>& ws, const std::filesystem::path& dir) {
    std::filesystem::create_directories(dir);
    for(const auto& w:ws) {
        auto path=dir/(w.name+".queries");
        if(std::filesystem::exists(path)) throw std::runtime_error("input dump already exists");
        std::ofstream f(path,std::ios::binary);
        for(const auto& q:w.input) for(auto v:{q.psm,q.tbp,q.tbw,q.x,q.y}) {
            const char b[4]={char(v),char(v>>8),char(v>>16),char(v>>24)};
            f.write(b,4);
        }
        if(!f) throw std::runtime_error("input dump failed");
    }
}
}
int main(int argc,char** argv) {
    try {
        if(argc<4) throw std::runtime_error("usage: okage-gpu-address-benchmark --verify|--baseline|--trial N|--dump-inputs DIR INPUT.gadr REFERENCE.bin");
        std::cout<<std::setprecision(12);
        std::string mode=argv[1];
        int pos=2,trial=0;
        if(mode=="--trial") { if(argc!=5) throw std::runtime_error("trial arguments"); trial=std::stoi(argv[pos++]); if(trial<0||trial>4) throw std::runtime_error("trial must be 0..4"); }
        else if(mode=="--dump-inputs") ++pos;
        else if(argc!=4 || (mode!="--verify" && mode!="--baseline")) throw std::runtime_error("invalid arguments");
        if(pos+2!=argc) throw std::runtime_error("invalid file arguments");
        auto input=corpus(argv[pos]); auto expected=reference(argv[pos+1],input.size());
        std::vector<Result> cpu(input.size()),scalar(input.size()),gpu(input.size());
        okage::texture_address_batch_cpu(input,cpu); okage::texture_address_batch_scalar(input,scalar);
        check(cpu,expected,"CPU/reference"); check(scalar,expected,"scalar/reference");
        auto ws=workloads(input);
        if(mode=="--dump-inputs") { dump_inputs(ws,argv[2]); return 0; }
        if(mode=="--baseline") {
            for(const auto& w:ws) {
                std::vector<Result> a(w.input.size()),b(w.input.size());
                for(int i=0;i<41;++i) {
                    double inline_s=0,scalar_s=0;
                    auto fast=[&] { auto s=Clock::now(); okage::texture_address_batch_cpu(w.input,a); auto e=Clock::now(); inline_s=seconds(s,e); };
                    auto slow=[&] { auto s=Clock::now(); okage::texture_address_batch_scalar(w.input,b); auto e=Clock::now(); scalar_s=seconds(s,e); };
                    if(i%2==0) { fast(); slow(); } else { slow(); fast(); }
                    check(a,b,"inline/scalar pilot");
                    if(i>=10) std::cout<<"{\"type\":\"baseline\",\"workload\":\""<<w.name<<"\",\"inline_first\":"<<(i%2==0?"true":"false")<<",\"inline_s\":"<<inline_s<<",\"scalar_s\":"<<scalar_s<<"}\n";
                }
            }
            return 0;
        }
        auto start=Clock::now();
        auto backend=okage::make_metal_texture_address_batch(okage::texture_address_batch_limit,mode=="--verify");
        auto end=Clock::now();
        // Name is an Apple device name; escape quotes/backslashes before JSON.
        auto name=backend->name(); std::string escaped;
        for(char c:name) { if(c=='"'||c=='\\') escaped+='\\'; if(c>=32) escaped+=c; }
        std::cout<<"{\"type\":\"setup\",\"device\":\""<<escaped<<"\",\"metal_factory_s\":"<<seconds(start,end)<<"}\n";
        auto first_corpus_start=Clock::now(); auto first_corpus=backend->execute(input,gpu); auto first_corpus_end=Clock::now();
        check(gpu,expected,"Metal/reference");
        std::cout<<"{\"type\":\"cold_first_execution\",\"count\":"<<input.size()<<",\"gpu_e2e_s\":"<<seconds(first_corpus_start,first_corpus_end)<<",\"gpu_command_s\":"; json_number(first_corpus.gpu_seconds); std::cout<<"}\n";
        std::cout<<"{\"type\":\"correctness\",\"rows\":"<<input.size()<<",\"different_results\":0}\n";
        if(mode=="--verify") {
            // Dedicated deterministic edge/status/guard/reuse tests are a separate executable.
            std::reverse(input.begin(),input.end()); std::reverse(expected.begin(),expected.end());
            std::fill(gpu.begin(),gpu.end(),Result{0xdeadbeef,0xdeadbeef});
            backend->execute(input,gpu); check(gpu,expected,"Metal changed-order reuse");
            std::cout<<"{\"type\":\"correctness_reuse\",\"rows\":"<<input.size()<<",\"different_results\":0}\n";
            return 0;
        }
        // Separate first maximum-capacity execution/allocation observation.
        const auto& cold=ws[ws.size()-2];
        auto alloc_start=Clock::now(); std::vector<Result> cold_out(cold.input.size()); auto alloc_end=Clock::now();
        auto first_start=Clock::now(); auto first=backend->execute(cold.input,cold_out); auto first_end=Clock::now();
        std::vector<Result> cold_expected(cold.input.size()); okage::texture_address_batch_cpu(cold.input,cold_expected); check(cold_out,cold_expected,"first max execution");
        std::cout<<"{\"type\":\"first_max_execution\",\"count\":"<<cold.input.size()<<",\"caller_allocation_s\":"<<seconds(alloc_start,alloc_end)<<",\"gpu_e2e_s\":"<<seconds(first_start,first_end)<<",\"gpu_command_s\":"; json_number(first.gpu_seconds); std::cout<<"}\n";
        for(const auto& w:ws) {
            std::vector<Result> a(w.input.size()),b(w.input.size()),exp(w.input.size());
            okage::texture_address_batch_cpu(w.input,exp);
            for(int pair=-10;pair<31;++pair) {
                double cpu_s=0,gpu_s=0,command_s=0;
                auto run_cpu=[&] { auto s=Clock::now(); okage::texture_address_batch_cpu(w.input,a); auto e=Clock::now(); cpu_s=seconds(s,e); check(a,exp,"CPU sample"); };
                auto run_gpu=[&] { auto s=Clock::now(); auto timing=backend->execute(w.input,b); auto e=Clock::now(); gpu_s=seconds(s,e); command_s=timing.gpu_seconds; check(b,exp,"GPU sample"); };
                bool cpu_first=((pair+10+trial)%2)==0;
                if(cpu_first) { run_cpu(); run_gpu(); } else { run_gpu(); run_cpu(); }
                if(pair>=0) {
                    std::cout<<"{\"type\":\"sample\",\"trial\":"<<trial<<",\"workload\":\""<<w.name<<"\",\"count\":"<<w.input.size()<<",\"input_bytes\":"<<w.input.size()*20<<",\"output_bytes\":"<<w.input.size()*8<<",\"pair\":"<<pair<<",\"cpu_first\":"<<(cpu_first?"true":"false")<<",\"cpu_s\":"<<cpu_s<<",\"gpu_e2e_s\":"<<gpu_s<<",\"gpu_command_s\":"; json_number(command_s); std::cout<<"}\n";
                }
            }
        }
        return 0;
    } catch(const std::exception& e) { std::cerr<<"gpu-address-benchmark: "<<e.what()<<'\n'; return 1; }
}
