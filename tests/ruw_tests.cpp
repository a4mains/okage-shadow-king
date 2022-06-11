#include "okage/ruw.hpp"
#include <algorithm>
#include <iostream>
#include <fstream>
#include <iterator>
#include <limits>
#include <string_view>
#include <stdexcept>

namespace {
void require(bool b){if(!b)throw std::runtime_error("RUW test failed");}
template<class F> void rejects(F f){try{f();}catch(const std::runtime_error&){return;}throw std::runtime_error("Expected RUW rejection");}
void put(std::vector<std::uint8_t>& b,unsigned at,std::uint32_t v) {
    for(unsigned i=0;i<4;++i)b.at(at+i)=v>>(i*8);
}

std::vector<std::uint8_t> mutable_fixture() {
    std::vector<std::uint8_t> b(40+9*68);
    put(b,12,1);put(b,16,4);put(b,20,1);put(b,36,8);
    const std::array<unsigned,8> ids={9,2,7,1,0,8,3,4};
    const std::array<unsigned,8> types={1,0,2,0,3,1,1,0};
    const std::array<unsigned,8> flags={0x61,0x41,0x40,1,0x41,0x41,0x41,0x41};
    const std::array<std::uint32_t,8> classes={0xffffffff,0xffffffff,0,0xffffffff,1,0xffffffff,2,3};
    for(unsigned i=0;i<8;++i) {
        const unsigned at=108+i*68;
        for(unsigned j=0;j<17;++j)put(b,at+4*j,0x1000+i*100+j);
        put(b,at+8,i==0?0x40102030:0x60102030);put(b,at+12,i==1?0:0x80203040);
        put(b,at+16,flags[i]);put(b,at+20,types[i]);put(b,at+32,classes[i]);
        put(b,at+40,i==1?1:0);put(b,at+60,ids[i]);
    }
    return b;
}
constexpr auto keep=okage::RuwResource::preserve_component;
void mutation_tests() {
    okage::RuwResource r(mutable_fixture());
    require(r.widget_index(3,9)==0&&r.widget_index(3,2)==1&&r.widget_index(3,0)==4);
    rejects([&]{(void)r.widget(4,0);});rejects([&]{(void)r.widget(3,-1);});
    rejects([&]{r.set_position(3,99,1,2);});
    const auto* owner=&*r.widget(3,9).runtime;
    const auto* text=r.widget(3,9).runtime->text->data();
    const auto untouched=r.widget(3,7).runtime->words;
    const auto raw_words=r.widget(3,9).words;
    auto expected_color_words=r.widget(3,9).runtime->words;
    expected_color_words[4]=(expected_color_words[4]&0xffffff)|0x40000000;
    expected_color_words[5]=(expected_color_words[5]&0xffffff)|0x40000000;
    r.set_class_rgba(3,-1,{keep,keep,keep,100});
    //First matching authored cap64 carries into later uncapped records.
    for(int id:{9,2,8}) {
        require(r.widget(3,id).runtime->words[4]>>24==64);
        require(r.widget(3,id).runtime->words[5]>>24==64);
    }
    require(r.widget(3,7).runtime->words==untouched); //Class-1 is not wildcard.
    require(!r.widget(3,1).runtime);
    require(r.widget(3,9).words==raw_words&&r.widget(3,9).runtime->words==expected_color_words);
    require(r.add_rgba(3,2,{keep,keep,keep,-256})==8);
    require(r.widget(3,2).runtime->words[4]>>24==0);
    require(r.widget(3,2).runtime->words[5]>>24==64);
    require(!(r.widget(3,2).words[4]&1));
    r.reset_widget(3,2);
    require(r.widget(3,2).runtime->words[4]==0x60102030&&(r.widget(3,2).words[4]&1));
    r.reset_class(3,-1);
    require(r.widget(3,2).runtime->words[4]==0&&!(r.widget(3,2).words[4]&1));
    require(r.widget(3,2).runtime->words[1]&1);
    require(&*r.widget(3,9).runtime==owner&&r.widget(3,9).runtime->text->data()==text);
    const auto before_position=r.widget(3,9).runtime->words;
    r.set_position(3,9,keep,359);r.set_runtime_word0(3,9,0x47);
    auto expected=before_position;expected[0]=0x47;expected[7]=359;
    require(r.widget(3,9).runtime->words==expected);
    r.set_position(3,9,-123,keep);require(r.widget(3,9).runtime->words[6]==std::uint32_t(-123));
    r.set_runtime_word0(3,1,0x47);r.set_position(3,1,1,2);r.reset_widget(3,1);
    require(r.add_rgba(3,1,{1,2,3,4})==0&&!r.widget(3,1).runtime);

    r.set_class_rgba(3,-1,{100,100,100,80});
    require(r.add_class_rgba(3,-1,{-200,200,0,128})==11);
    require(r.widget(3,9).runtime->words[4]==0x4064ff00);
    require(r.widget(3,2).runtime->words[4]==0x8064ff00);
    require(r.widget(3,2).runtime->words[5]>>24==128);
    require(r.add_class_rgba(3,1234,{1,2,3,4})==0);
    r.set_class_rgba(3,0,{100,keep,keep,200}); //Set does not apply add's alpha128 cap.
    require(r.widget(3,7).runtime->words[4]>>24==200);
    require(!(r.widget(3,7).words[4]&1)); //Without8000 the show flag is untouched.
    require(r.add_rgba(3,7,{std::numeric_limits<std::int32_t>::max(),keep,keep,keep})==1);
    require((r.widget(3,7).runtime->words[4]&255)==0); //ADDU wraps before signed clamp.
    r.set_class_rgba(3,0,{-1,keep,keep,keep});
    require(r.widget(3,7).runtime->words[4]==0xffffffff); //Set's channel input is not byte-masked.
    r.set_class_rgba(3,-1,{keep,keep,keep,-1});
    require(r.widget(3,9).runtime->words[4]>>24==64&&r.widget(3,8).runtime->words[4]>>24==64);
    require(r.add_class_rgba(3,0,{keep,keep,keep,0})==8); //255 clamps to128 even with delta0.
    require(r.widget(3,7).runtime->words[4]==0x80ffffff);
    require(!r.widget(3,7).runtime->text);

    std::string input="abcdef";r.set_text(3,9,input);input.assign("changed");
    require(std::string(text)=="abcdef");
    r.set_text(3,9,std::string_view(text+2,4));require(std::string(text)=="cdef"); //Alias-safe ownership.
    r.set_text(3,9,std::string_view("Z\0ignored",9));require(text[0]=='Z'&&text[1]==0&&text[2]=='e');
    r.reset_class(3,-1);r.reset_widget(3,9);
    require(r.widget(3,9).runtime->text->data()==text&&text[0]=='Z'&&text[2]=='e');
    r.set_text(3,9,std::string(127,'x'));const auto full=*r.widget(3,9).runtime->text;
    rejects([&]{r.set_text(3,9,std::string(128,'y'));});require(*r.widget(3,9).runtime->text==full);
    r.set_text(3,9,{});require(text[0]==0&&text[1]=='x');
    r.set_text(3,1,"no owner");r.set_text(3,7,"nontext owner");
    require(!r.widget(3,1).runtime&&!r.widget(3,7).runtime->text);
    auto copy=r;r.set_text(3,9,"original");
    require(copy.widget(3,9).runtime->text->data()!=text&&copy.widget(3,9).runtime->text->at(0)==0);
    require(r.widget(3,9).words[16]==0);
}
void diagnostic(const char* path) {
    std::ifstream stream(path,std::ios::binary);if(!stream)throw std::runtime_error("Cannot read owned BTL.RUW");
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(stream)),{});okage::RuwResource r(bytes);
    require(r.widgets().size()==6&&r.widgets()[3].size()==72&&r.widgets()[5].size()==57);
    bool reordered=false;for(unsigned i=0;i<r.widgets()[3].size();++i)
        if(r.widgets()[3][i].words[15]!=i)reordered=true;
    require(reordered);
    for(unsigned i=0;i<r.widgets()[3].size();++i)
        require(r.widget(3,r.widgets()[3][i].words[15]).words[15]==r.widgets()[3][i].words[15]);
    r.reset_class(3,1);r.reset_class(3,2);r.reset_widget(4,1);r.reset_widget(2,9);
    r.add_rgba(4,1,{keep,keep,keep,-256});require((r.widget(4,1).runtime->words[4]>>24)==0);
    for(int id=29;id<=33;++id)r.set_text(5,id,"bounded owned-resource diagnostic");
    for(int id=34;id<=38;++id){r.set_runtime_word0(5,id,0x47);require(r.widget(5,id).runtime->words[0]==0x47);}
    for(int id=48;id<=51;++id){const auto x=r.widget(5,id).runtime->words[6];r.set_position(5,id,keep,359+(id-48)*17);require(r.widget(5,id).runtime->words[6]==x);}
}
}
int main(int argc,char** argv){try {
    if(argc==3&&std::string_view(argv[1])=="--diagnostic") {
        diagnostic(argv[2]);std::cout<<"Owned RUW mutation diagnostic passed\n";return 0;
    }
    require(argc==1);mutation_tests();
    std::vector<std::uint8_t> b(32+16+7*68);
    put(b,8,1);put(b,12,1);put(b,16,2);
    put(b,20,1);put(b,24,2);put(b,28,4);
    for(unsigned i=0;i<4;++i)put(b,32+i*4,100+i);
    for(unsigned i=0;i<7;++i)for(unsigned j=0;j<17;++j)put(b,48+i*68+j*4,1000+i*100+j);
    const std::array<unsigned,6> types={0,1,2,3,0,4},flags={1,1,0x41,0x40,0,1};
    for(unsigned i=0;i<6;++i){put(b,116+i*68+16,flags[i]);put(b,116+i*68+20,types[i]);}
    okage::RuwResource r(b);b.assign(b.size(),0); // All ownership is independent.
    require(r.styles().size()==1&&r.styles()[0][3]==103);
    require(r.templates()[0][0].words[16]==1016&&!r.templates()[0][0].runtime);
    const auto& first=r.widgets()[0][0];const auto& text=r.widgets()[0][1];
    require(!first.runtime&&first.words[4]==0x8001&&first.words[16]==0);
    require(text.runtime&&text.runtime->text&&text.runtime->words[1]==1&&text.words[4]==0x8001);
    require(std::string(text.runtime->text->data())=="\\f[1]abcdefg");
    require(text.runtime->words==std::array<std::uint32_t,10>{1207,1,1200,1201,1202,1203,1211,1212,1213,1214});
    require(r.widgets()[1][0].runtime&&!r.widgets()[1][0].runtime->text);
    require(r.widgets()[1][1].runtime&&!r.widgets()[1][1].runtime->text);
    require(!r.widgets()[1][2].runtime&&!r.widgets()[1][3].runtime);
    require(r.widgets()[1][3].words[4]==1&&r.widgets()[1][3].words[16]==1616);
    put(b,8,1);put(b,12,1);put(b,16,1);put(b,20,1);put(b,24,1);
    for(unsigned n:{0,16,24,40,100,179})rejects([&]{okage::RuwResource bad(okage::Bytes(b).first(n));});
    put(b,24,0xffff);rejects([&]{okage::RuwResource bad(b);});
    put(b,24,0x7fff);rejects([&]{okage::RuwResource bad(b);});
    put(b,12,0);rejects([&]{okage::RuwResource bad(b);});
    std::cout<<"RUW tests passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
