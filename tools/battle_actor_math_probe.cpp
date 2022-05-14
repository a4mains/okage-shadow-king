#include "okage/battle_actor_math.hpp"
#include <array>
#include <algorithm>

#include <bit>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <vector>
#include <fcntl.h>
#include <unistd.h>

int main(int argc,char** argv) {
    try {
        if(argc!=3)throw std::runtime_error("Usage: battle-actor-math-probe INPUT OUTPUT");
        if(std::filesystem::exists(argv[2])||std::filesystem::is_symlink(argv[2]))
            throw std::runtime_error("Refusing existing probe output");
        std::ifstream file(argv[1],std::ios::binary|std::ios::ate);
        if(!file)throw std::runtime_error("Cannot read probe input");
        const auto size=file.tellg();
        if(size<=0||size>64*224||size%224)throw std::runtime_error("Probe input extent");
        std::vector<std::uint8_t> input(static_cast<std::size_t>(size));file.seekg(0);
        if(!file.read(reinterpret_cast<char*>(input.data()),size))throw std::runtime_error("Short probe input");
        auto word=[&](std::size_t at){return std::uint32_t(input[at])|(std::uint32_t(input[at+1])<<8)|
            (std::uint32_t(input[at+2])<<16)|(std::uint32_t(input[at+3])<<24);};
        std::vector<std::uint8_t> result;std::set<std::uint32_t> ids;
        auto put=[&](std::uint32_t value){for(unsigned i=0;i<4;++i)result.push_back(value>>(8*i));};
        for(std::size_t at=0;at<input.size();at+=224) {
            const auto id=word(at),op=word(at+4),drawCount=word(at+28);
            if(!ids.insert(id).second||op>1||drawCount>16)throw std::runtime_error("Probe row identity/operation/RNG bounds");
            const auto parameter=__builtin_bit_cast(std::int32_t, (word(at+8)));
            const auto kind=word(at+12),entryBits=word(at+16),formation=word(at+24);
            const auto entry=__builtin_bit_cast(std::int32_t, (entryBits));
            if(op==0&&(kind>1||entry<-32768||entry>32767||word(at+20)||formation))
                throw std::runtime_error("Probe timer parameters");
            if(op==1&&(parameter||kind||entry||formation>65535))throw std::runtime_error("Probe selector parameters");
            for(unsigned i=drawCount;i<16;++i)if(word(at+32+4*i))throw std::runtime_error("Probe unused RNG words must be zero");
            std::array<std::uint8_t,128> body;std::copy_n(input.begin()+at+96,128,body.begin());
            unsigned consumed=0;
            const auto random=[&](){
                if(consumed==drawCount)throw std::runtime_error("Probe RNG sequence exhausted");
                return word(at+32+4*consumed++);
            };
            auto storage=std::span<std::uint8_t>(body).subspan(16,96);
            if(op==0)okage::initialize_battle_actor_timer(storage,parameter,kind,entry,random);
            else okage::initialize_battle_enemy_selector(storage,__builtin_bit_cast(std::int32_t, (word(at+20))),formation,random);
            put(id);put(op);put(consumed);put(0);result.insert(result.end(),body.begin(),body.end());
        }
        const int fd=::open(argv[2],O_WRONLY|O_CREAT|O_EXCL,0600);
        if(fd<0)throw std::runtime_error("Cannot create fresh probe output");
        std::size_t at=0;
        while(at<result.size()) {
            const auto n=::write(fd,result.data()+at,result.size()-at);
            if(n<=0){::close(fd);throw std::runtime_error("Cannot write probe output");}
            at+=std::size_t(n);
        }
        if(::close(fd))throw std::runtime_error("Cannot close probe output");
        std::cout<<input.size()/224<<" arithmetic records written\n";return 0;
    } catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
