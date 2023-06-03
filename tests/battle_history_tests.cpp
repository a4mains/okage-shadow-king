#include "okage/battle_hud.hpp"
#include "okage/battle_data.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
template<class F> void rejects(F&& f) {try{f();}catch(const std::runtime_error&){return;}throw std::runtime_error("expected battle history rejection");}
void put(std::vector<std::uint8_t>& bytes,std::size_t at,std::uint32_t value) {
    for(unsigned i=0;i<4;++i)bytes.at(at+i)=std::uint8_t(value>>(i*8));
}

std::vector<std::uint8_t> make_xdb(bool battle) {
    std::vector<std::uint8_t> strings;
    auto add=[&](std::string_view s) {const auto offset=std::uint32_t(strings.size());strings.insert(strings.end(),s.begin(),s.end());strings.push_back(0);return offset;};
    const auto section=add(battle?"unit":"item");
    const auto sysmes=add("sysmes");
    const unsigned rows=battle?170:1,columns=battle?27:15;
    std::vector<std::uint32_t> words(rows*columns);
    for(unsigned row=0;row<rows;++row) {
        words[row*columns]=add(row==0?"Slime":row==1?"Orc":"Unit");
        words[row*columns+(battle?1:14)]=add("model");
    }
    const auto record_words=std::uint32_t(words.size());
    if(battle)for(unsigned i=0;i<=0xab;++i) {
        std::string_view text="";
        if(i==0xa6)text="     ";
        if(i==0xa7)text="Hit:%s";
        if(i==0xa9)text="%s(%c)";
        if(i==0xaa)text="No value";
        if(i==0xab)text="Amount:%s";
        words.push_back(add(text));
    }
    const unsigned sections=battle?2:1;
    const std::size_t base=32+sections*16;
    std::vector<std::uint8_t> result(base+words.size()*4+strings.size());
    put(result,0,0x424458);put(result,4,std::uint32_t(words.size()*4));put(result,8,std::uint32_t(strings.size()));
    put(result,12,sections);put(result,16,0xffffffff);
    put(result,32,section);put(result,36,0);put(result,40,record_words);put(result,44,0);
    if(battle) {put(result,48,sysmes);put(result,52,record_words*4);put(result,56,std::uint32_t(words.size())-record_words);put(result,60,0);}
    for(unsigned i=0;i<words.size();++i)put(result,base+i*4,words[i]);
    std::copy(strings.begin(),strings.end(),result.begin()+base+words.size()*4);
    return result;
}
okage::GameDataTables tables() {return {make_xdb(true),make_xdb(false)};}

std::vector<std::uint8_t> make_hud_data() {
    std::vector<std::uint8_t> bytes(480);
    const std::string magic="OKBHUD2";std::copy(magic.begin(),magic.end(),bytes.begin());bytes[7]=0;
    put(bytes,8,2);put(bytes,12,400);
    const std::string hash="2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5";
    for(unsigned i=0;i<32;++i)bytes[16+i]=std::uint8_t(std::stoul(hash.substr(i*2,2),nullptr,16));
    for(unsigned i=0;i<18;++i)put(bytes,80+i*4,i+1);
    const auto ratio=std::bit_cast<std::uint64_t>(0.1);put(bytes,152,std::uint32_t(ratio));put(bytes,156,std::uint32_t(ratio>>32));
    unsigned at=160;
    for(const std::string text:{"\\f[1]\\c[%d]00\\c[0]0","%03d","\\f[1]\\c[%d]%s\\c[0]%s","\\f[1]\\c[%d]0\\c[0]0","%02d"}) {
        std::copy(text.begin(),text.end(),bytes.begin()+at);at+=64;
    }
    return bytes;
}

std::vector<std::uint8_t> make_battle_data(std::span<const okage::BattleFormationEntry> entries) {
    constexpr std::size_t header=56,directory=256*8,stages=23*64;
    std::vector<std::uint8_t> bytes(header+directory+stages+entries.size()*12);
    std::copy_n("OKBTDAT1",8,bytes.begin());put(bytes,8,1);put(bytes,12,256);put(bytes,16,23);put(bytes,20,std::uint32_t(entries.size()));
    const std::string hash="2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5";
    for(unsigned i=0;i<32;++i)bytes[24+i]=std::uint8_t(std::stoul(hash.substr(i*2,2),nullptr,16));
    for(unsigned i=0;i<256;++i)put(bytes,header+i*8+4,0xffffffff);
    if(!entries.empty()){put(bytes,header,0);put(bytes,header+4,std::uint32_t(entries.size()));}
    for(unsigned i=0;i<23;++i) {
        const std::string path="btldata/beta2/field/stage"+std::to_string(i+1)+".xpf";
        std::copy(path.begin(),path.end(),bytes.begin()+header+directory+i*64);
    }
    std::size_t at=header+directory+stages;
    for(const auto& entry:entries) {
        bytes[at]=std::uint8_t(entry.unit_id);bytes[at+1]=std::uint8_t(std::uint16_t(entry.unit_id)>>8);
        bytes[at+2]=std::uint8_t(entry.field_2);bytes[at+3]=std::uint8_t(entry.field_3);
        put(bytes,at+4,std::bit_cast<std::uint32_t>(entry.x));put(bytes,at+8,std::bit_cast<std::uint32_t>(entry.z));at+=12;
    }
    return bytes;
}
std::vector<std::uint8_t> make_ruw(bool text_owners=true) {
    std::vector<std::uint8_t> bytes(48);
    put(bytes,8,0);put(bytes,12,1);put(bytes,16,6);put(bytes,20,0);
    for(unsigned group=0;group<6;++group)put(bytes,24+group*4,group==5?60:0);
    for(unsigned id=0;id<60;++id) {
        const auto at=bytes.size();bytes.resize(at+68);
        put(bytes,at+8,0x7f203040);put(bytes,at+12,0x7f506070);
        put(bytes,at+16,0x41);put(bytes,at+20,(text_owners||id<5)?1:2);
        put(bytes,at+32,1);put(bytes,at+52,120);put(bytes,at+56,80);put(bytes,at+60,id);
    }
    return bytes;
}
std::string text(const okage::RuwResource& r,int id) {return r.widget(5,id).runtime->text->data();}

void event_zero_direct_name() {
    auto t=tables();okage::BattleHudData data(make_hud_data());
    constexpr std::array formation{okage::BattleFormationEntry{1,1,0,0,0},okage::BattleFormationEntry{2,2,0,0,0}};
    const auto artifact=make_battle_data(formation);const okage::BattleData battle(artifact);
    okage::RuwResource r(make_ruw());okage::BattleHud hud;hud.initialize_type3(r,t);
    hud.append_history_event(r,data,t,battle,0,0,0xaa,-23,true,"Slime");
    require(hud.type3_slots[0]==0xaa&&hud.type3_slots[1]==0x16&&hud.type3_slots[2]==-23,"kind0 source triplet");
    require(hud.type3_cursor==1&&hud.type3_animation==1,"source cursor and animation word");
    require(text(r,29)=="Hit:Slime"&&text(r,39)=="Amount:\\f[1]\\c[510543555]0\\c[0]23","real formatted row strings");
    require(r.widget(5,39).runtime->words[4]==0x7f202080,"negative-value source color");
    require(r.widget(5,34).runtime->words[4]==0x7fffffff,"number style alpha/color");
    require(r.widget(5,29).runtime->words[6]==0x120&&r.widget(5,29).runtime->words[7]==0x15b,"message layout position");
    require(r.widget(5,34).runtime->words[6]==0x1d8&&r.widget(5,34).runtime->words[7]==0x158,"style layout position");
    require(std::string(hud.type3_current_number.data())=="\\f[1]\\c[510543555]0\\c[0]23","shared 1240c8 scratch");
    require(r.widget(5,30).runtime->words[4]>>24==0,"empty row hidden");
}

void event_zero_formation_lookup() {
    auto t=tables();okage::BattleHudData data(make_hud_data());
    constexpr std::array formation{okage::BattleFormationEntry{1,1,0,0,0},okage::BattleFormationEntry{1,1,0,0,0}};
    const auto artifact=make_battle_data(formation);const okage::BattleData battle(artifact);
    okage::RuwResource r(make_ruw());okage::BattleHud hud;hud.initialize_type3(r,t);
    hud.append_history_event(r,data,t,battle,0,0,0x11,0,true);
    require(text(r,29)=="Slime(B)","group and duplicate character formatting");
    require(text(r,39)=="No value","zero value branch keeps authored message");
    require(r.widget(5,29).runtime->words[4]==0x7f963cdc,"formation message palette");
}

void event_five_and_ring() {
    auto t=tables();okage::BattleHudData data(make_hud_data());
    constexpr std::array formation{okage::BattleFormationEntry{1,1,0,0,0}};
    const auto artifact=make_battle_data(formation);const okage::BattleData battle(artifact);
    okage::RuwResource r(make_ruw());okage::BattleHud hud;hud.initialize_type3(r,t);
    rejects([&]{hud.append_history_event(r,data,t,battle,0,5,0xaa,77,false,"Slime");});
    require(hud.type3_slots[0]==-1&&hud.type3_animation==0,"unknown stack residue rejects transactionally");
    for(int i=1;i<=6;++i)hud.append_history_event(r,data,t,battle,0,i==5?5:0,0xaa,-i,true,"Slime");
    require(hud.type3_slots[0]==0xaa&&hud.type3_slots[2]==-3,"full history shifts oldest entry");
    require(hud.type3_slots[7]==0x56&&hud.type3_slots[8]==-5,"death event uses source kind style and explicit value");
    require(hud.type3_slots[12]==0xaa&&hud.type3_slots[14]==-6,"latest row remains at cursor after source shift");
    require(hud.type3_cursor==4,"history cursor saturates at final row");
}

void unsupported_inputs_are_atomic() {
    auto t=tables();okage::BattleHudData data(make_hud_data());
    constexpr std::array formation{okage::BattleFormationEntry{1,1,0,0,0}};
    const auto artifact=make_battle_data(formation);const okage::BattleData battle(artifact);
    okage::RuwResource r(make_ruw());okage::BattleHud hud;hud.initialize_type3(r,t);
    const auto slots=hud.type3_slots;const auto row=text(r,29);
    rejects([&]{hud.append_history_event(r,data,t,battle,0,7,0xaa,1,true,"Slime");});
    require(hud.type3_slots==slots&&text(r,29)==row&&hud.type3_animation==0,"unsupported event kind rejects before mutation");
    rejects([&]{hud.append_history_event(r,data,t,battle,0,0,0x20,1,true);});
    require(hud.type3_slots[0]==0x20&&hud.type3_animation==1,"source writes the event before a later group lookup fails");
    okage::RuwResource r2(make_ruw());okage::BattleHud hud2;hud2.initialize_type3(r2,t);
    rejects([&]{hud2.append_history_event(r2,data,t,battle,0,0,0xaa,1,true);});
    require(hud2.type3_slots[0]==0xaa&&hud2.type3_animation==1,"late special-name guard retains source-ordered history stores");
}
}

int main() {
    try {event_zero_direct_name();event_zero_formation_lookup();event_five_and_ring();unsupported_inputs_are_atomic();}
    catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 1;}
    return 0;
}
