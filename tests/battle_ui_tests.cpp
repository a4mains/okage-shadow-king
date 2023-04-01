#include "okage/battle_ui.hpp"
#include "okage/event.hpp"
#include "okage/ui_window.hpp"
#include "okage/battle_interface.hpp"
#include "okage/battle_actor.hpp"
#include <algorithm>
#include <bit>
#include <cfenv>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string_view>

namespace {
void require(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
void put(std::vector<std::uint8_t>& b,unsigned at,std::uint32_t w) {
    for(unsigned i=0;i<4;++i)b.at(at+i)=w>>(8*i);
}
void digest(std::vector<std::uint8_t>& b,unsigned at,std::string_view s) {
    constexpr std::string_view digits="0123456789abcdef";
    for(unsigned i=0;i<s.size()/2;++i)b.at(at+i)=digits.find(s[i*2])*16+digits.find(s[i*2+1]);
}
std::vector<std::uint8_t> fixture() {
    std::vector<std::uint8_t> b(312);
    const std::string_view magic="OKBTUI1";std::copy(magic.begin(),magic.end(),b.begin());
    put(b,8,1);put(b,12,2);put(b,16,28);put(b,20,112);
    digest(b,24,"2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5");
    digest(b,56,"5632c521fdd835a20c81960f9758447216733e2de4afc2d0e32b05ace9e8805b");
    for(unsigned i=0;i<2;++i) {
        const unsigned at=88+112*i;
        put(b,at,i+1);put(b,at+4,40);put(b,at+8,6);put(b,at+12,2);
        put(b,at+16,0x80000000);put(b,at+64,0x80808080);
        put(b,at+80,1);put(b,at+84,1);put(b,at+88,i+1);put(b,at+92,i+1);
        put(b,at+96,i+2);put(b,at+100,i?13:29);put(b,at+104,i?8:17);
    }
    return b;
}
void rejects(const std::vector<std::uint8_t>& b) {
    try {okage::BattleUiData data(b);}
    catch(const std::runtime_error&) {return;}
    throw std::runtime_error("invalid interface accepted");
}
void first_dispatch() {
    auto b=fixture();
    put(b,88+0x24,0xbfc00000);put(b,88+112+0x24,0xbfc00000);
    const okage::BattleUiData data(b);
    std::vector<std::uint8_t> empty_ruw(48);put(empty_ruw,12,1);put(empty_ruw,16,6);
    auto resource=std::make_shared<okage::RuwResource>(empty_ruw);
    okage::BattleUiShared shared;
    okage::BattleInterface ui(shared,data,std::make_shared<int>(1),0,[](unsigned){});
    okage::BattleUiPad pad;okage::BattleUiPadInput input{};
    const int rounding=std::fegetround();std::fesetround(FE_UPWARD);
    shared.resource_accumulators.fill(123);
    ui.first_update(resource,pad,input,60);
    require(std::fegetround()==FE_UPWARD,"first dispatcher restores host rounding");
    const auto& words=ui.words();
    require(words[0xe4c/4]==0xffffff9c&&words[0xe50/4]==1,"type2 does not fall through; type3 does");
    require(words[0xe8c/4]==2&&words[0xe48/4]==1,"state cursor and layer order");
    require(words[0x114/4]==1&&words[0x1f4/4]==1,"typed callback manager links");
    require(words[0xa4/4]==19&&words[0x184/4]==0,"initial commands");
    require(words[0x64/4]==0xbfc00000&&words[0x144/4]==0xbfb94c5d,"rotation only type3 with source single rounding");
    require(pad.controls[0].delay==1800&&pad.controls[0].interval==900,"source repeat settings multiply by display rate");
    require(ui.window(1).word(0x554)==words[0x144/4],"type3 rotation reaches window");
    require(shared.resource_accumulators[0]==0&&shared.resource_accumulators[1]==123,"only resource slot0 reset");
    require(ui.window(0).opacity()==128&&ui.window(1).opacity()==128,"callback sets alpha without advancing clock");
    ui.first_update(resource,pad,input,60);
    require(words[0xe4c/4]==2&&words[0xa4/4]==0,"type2 -100 advances to 2");
    bool guarded=false;
    try{ui.first_update(resource,pad,input,60);}catch(const std::runtime_error&){guarded=true;}
    require(guarded&&std::fegetround()==FE_UPWARD,"visible interactive type2 remains guarded and restores rounding");
    shared.registry[0]->set_opacity_tween(0,0,0,128);
    const auto flags=ui.window(0).word(0x1900);
    shared.registry[0]->set_layout_enabled(false);
    require(ui.window(0).word(0x1900)==(flags&~0x1000u),"content flag clears only source bit");
    shared.registry[0]->set_layout_enabled(true);
    require(ui.window(0).word(0x1900)==(flags|0x1000u),"content flag sets only source bit");
    input.left_x=0;shared.hud.selection_state=51;
    const auto masks=ui.first_update(resource,pad,input,60);
    require(pad.analog_x_ticks==1&&masks.repeat==0,"inhibition follows unconditional analog update");
    input.left_x=128;shared.hud.selection_state=0;
    for(unsigned i=0;i<40;++i)ui.first_update(resource,pad,input,60);
    require(words[0xe50/4]==2&&words[0x144/4]==0&&ui.window(1).word(0x554)==0,"source rotation completes on positive crossing");
    require(words[0xe4c/4]==2&&words[0xa4/4]==0xffffffff,"zero-opacity type2 skips interaction");
    const auto other_opacity=ui.window(0).opacity();
    const auto prior_words=ui.words();
    ui.reveal_layer(1,16);
    require(ui.window(1).opacity()==1&&ui.window(1).word(0x27c)==0x41800000,
            "parent reveal starts at one without prematurely advancing resource clock");
    require(ui.window(1).word(0x274)==0x43000000&&ui.window(1).word(0x278)==0,
            "parent reveal uses source opacity bounds");
    require(ui.window(0).opacity()==other_opacity&&ui.words()==prior_words,
            "parent reveal preserves sibling and interface dispatch state");
    bool invalid_layer=false;
    try{ui.reveal_layer(2,16);}catch(const std::runtime_error&){invalid_layer=true;}
    require(invalid_layer&&ui.window(1).opacity()==1,"invalid reveal cannot mutate an owned layer");
    std::fesetround(rounding);
}
void opening_selection() {
    const okage::BattleUiData data(fixture());
    std::vector<std::uint8_t> empty_ruw(48);put(empty_ruw,12,1);put(empty_ruw,16,6);
    auto resource=std::make_shared<okage::RuwResource>(empty_ruw);
    okage::BattleUiShared shared;
    okage::BattleInterface ui(shared,data,std::make_shared<int>(1),0,[](unsigned){});
    auto& words=const_cast<std::array<std::uint32_t,0xef0/4>&>(ui.words());
    std::array<okage::BattleActor,3> actors;
    std::array<okage::BattleEntityBinding,3> entities{};
    for(unsigned i:{0u,2u}) {
        entities[i].actor=&actors[i];
        actors[i].storage[0x21c]=2;actors[i].storage[0x220]=2;
    }
    const okage::BattleUiFirstContext context{3,2,entities};
    shared.hud.type2_work.fill(0xffffffffu);
    shared.hud.type2_work[128]=3;shared.hud.type2_work[131]=0;
    shared.hud.type2_work[139]=0;
    shared.hud.type2_work[144]=7;
    words[0xd0/4]=23;words[0xd4/4]=24;words[0xd8/4]=25;
    words[0xe8/4]=77;words[0x64/4]=0xbfc00000;
    const auto remembered=shared.hud.type2_work;
    ui.open_layer(0);okage::BattleUiPad pad;
    const auto rounding=std::fegetround();std::fesetround(FE_UPWARD);
    ui.first_update(resource,pad,{},60,&context);
    require(std::fegetround()==FE_UPWARD,"opening restores host rounding");
    std::fesetround(rounding);
    require(words[0xe4c/4]==2&&words[0xe8/4]==1,"opening chooses compact ready ordinal, not entity ID");
    require(words[0x64/4]==0&&ui.window(0).word(0x554)==0,"opening publishes cleared rotation");
    require(words[0xcc/4]==0&&words[0xd0/4]==23&&words[0xd4/4]==0&&words[0xd8/4]==25,
            "opening reloads category and preserves sentinel-marked selections");
    require(shared.hud.type2_work==remembered,"opening reads persistent selections without resetting cache");
    require(ui.window(0).opacity()==1,"opening callback does not advance resource clock");
    entities[2].actor=nullptr;words[0xe8/4]=9;ui.open_layer(0);
    ui.first_update(resource,pad,{},60,&context);
    require(words[0xe8/4]==9,"no matching ready actor preserves source ordinal");
    const okage::BattleUiFirstContext demo{4,-1,{}};
    words[0xcc/4]=3;words[0xd0/4]=19;ui.open_layer(0);
    ui.first_update(resource,pad,{},60,&demo);
    require(words[0xe8/4]==0&&words[0xcc/4]==3&&words[0xd0/4]==19,
            "demo opening resets ordinal without reading party or remembered selections");
}
void category_input() {
    const okage::BattleUiData data(fixture());
    std::vector<std::uint8_t> empty_ruw(48);put(empty_ruw,12,1);put(empty_ruw,16,6);
    auto resource=std::make_shared<okage::RuwResource>(empty_ruw);
    okage::BattleUiShared shared;
    okage::BattleInterface ui(shared,data,std::make_shared<int>(1),0,[](unsigned){});
    auto& words=const_cast<std::array<std::uint32_t,0xef0/4>&>(ui.words());
    std::array<okage::BattleActor,3> actors;
    std::array<okage::BattleEntityBinding,3> entities{};
    for(unsigned i:{0u,2u}){entities[i].actor=&actors[i];actors[i].storage[0x21c]=2;actors[i].storage[0x220]=2;}
    std::vector<std::uint8_t> variables(0xd78);okage::UiPartySelection party;party.refresh(variables,3);
    std::vector<unsigned> sounds;
    okage::BattleUiFirstContext context{3,0,entities,variables,nullptr,&party,[&](unsigned id){sounds.push_back(id);}};
    okage::BattleUiPad pad;ui.open_layer(0);ui.first_update(resource,pad,{},60,&context);
    words[0xd0/4]=77;words[0xe4/4]=88;
    okage::BattleUiPadInput input;input.held=0x4000;input.repeat=0x4000;input.pressed=8;
    ui.first_update(resource,pad,input,60,&context);
    require(words[0xe4c/4]==2&&words[0xa4/4]==0xffffffff&&words[0xa8/4]==0,"navigation stays in actual category state");
    require(words[0xe8/4]==1&&words[0xc8/4]==1,"party switch publishes ready ordinal to parent argument7");
    require(words[0xcc/4]==3&&words[0xac/4]==1,"eligibility forces category3 without overwriting earlier directional event");
    require(words[0xd0/4]==0&&words[0xe4/4]==0&&words[0xb0/4]==0xffffffff,"category update clears six selections and eight event arguments");
    require(shared.hud.type2_enabled==0&&sounds==std::vector<unsigned>{0,0},"selection and category sounds follow source order");
    shared.hud.selection_state=51;input.pressed=0x40;
    ui.first_update(resource,pad,input,60,&context);
    require(words[0xe4c/4]==2&&words[0xac/4]==0xffffffff&&sounds.size()==2,"shared countdown inhibits normalized input but still runs category update");
    shared.hud.selection_state=0;input={};input.repeat=0x40;
    ui.first_update(resource,pad,input,60,&context);
    require(words[0xe4c/4]==2,"confirm consumes pressed edge, not repeat");
    input={};input.pressed=0x40;
    ui.first_update(resource,pad,input,60,&context);
    require(words[0xe4c/4]==3&&words[0xa4/4]==16&&words[0xa8/4]==16,"confirm advances to actual next menu command");
    require(shared.hud.type2_work[0]==3&&sounds.back()==3,"confirm persists category and plays owned confirmation cue");
}
void attack_submenu_input() {
    const okage::BattleUiData data(fixture());
    std::vector<std::uint8_t> empty_ruw(48);put(empty_ruw,12,1);put(empty_ruw,16,6);
    auto resource=std::make_shared<okage::RuwResource>(empty_ruw);
    okage::BattleUiShared shared;
    okage::BattleInterface ui(shared,data,std::make_shared<int>(1),0,[](unsigned){});
    auto& words=const_cast<std::array<std::uint32_t,0xef0/4>&>(ui.words());
    std::array<okage::BattleActor,3> actors;
    std::array<okage::BattleEntityBinding,3> entities{};
    entities[0].actor=&actors[0];actors[0].storage[0x21c]=2;actors[0].storage[0x220]=2;
    std::vector<unsigned> sounds;
    okage::BattleUiFirstContext context{3,0,entities,{},nullptr,nullptr,[&](unsigned id){sounds.push_back(id);}};
    okage::BattleUiPad pad;
    words[0xe4c/4]=3;words[0xa8/4]=1;words[0xd0/4]=9;
    shared.hud.type2_flag_a=7;shared.hud.type2_flag_b=8;
    okage::BattleUiPadInput input;input.pressed=0x20;input.repeat=0x4000;
    ui.first_update(resource,pad,input,60,&context);
    require(words[0xa4/4]==2&&words[0xa8/4]==2&&words[0xd0/4]==0&&words[0xb0/4]==0,
        "single ready actor automatically opens targeting with real option0");
    require(words[0xe4c/4]==3&&sounds.empty(),"automatic target opening returns before input/cancel tail");
    require(shared.hud.type2_flag_a==0&&shared.hud.type2_flag_b==0,"Attack resets shared target flags");
    shared.formation.count=3;words[0xd4/4]=0;
    input={};input.repeat=0x1000;
    ui.first_update(resource,pad,input,60,&context);
    require(shared.target_count==3&&words[0xd4/4]==2&&words[0xb4/4]==2&&words[0xa4/4]==0xffffffff,
        "target up wraps actual formation group count and publishes column2");
    input={};input.pressed=0x40;input.repeat=0x4000;
    ui.first_update(resource,pad,input,60,&context);
    require(words[0xa4/4]==5&&words[0xa8/4]==5&&shared.hud.type2_work[16]==2,
        "target confirmation remembers pre-navigation index in category-specific cache");
    require(words[0xd4/4]==0&&words[0xb4/4]==0&&sounds==std::vector<unsigned>{0,3,0},
        "generic tail runs repeat after confirm and preserves sound ordering");
    words[0xa8/4]=2;words[0xd4/4]=2;input={};input.pressed=0x20;
    ui.first_update(resource,pad,input,60,&context);
    require(words[0xa4/4]==0xffffff9c&&words[0xa8/4]==0&&words[0xe4c/4]==1&&words[0xac/4]==0,
        "single actor target cancel returns through source category opening state");
    require(sounds.back()==4,"target cancellation plays source cancel cue");
    words[0xe4c/4]=3;words[0xa8/4]=2;shared.formation.count=0;
    input={};input.pressed=0x40;
    const auto before=sounds.size();ui.first_update(resource,pad,input,60,&context);
    require(words[0xa4/4]==0xffffffff&&words[0xa8/4]==2&&sounds.size()==before,
        "empty target roster cannot confirm or play success sound");
    entities[2].actor=&actors[2];actors[2].storage[0x21c]=2;actors[2].storage[0x220]=2;
    words[0xa8/4]=1;words[0xd0/4]=1;input={};input.repeat=0x4000;
    ui.first_update(resource,pad,input,60,&context);
    require(words[0xa8/4]==1&&words[0xd0/4]==0,"multiple ready actors expose two ordinary attack options");
    actors[0].storage[0x17c]=1;words[0xd0/4]=1;
    ui.first_update(resource,pad,input,60,&context);
    require(words[0xd0/4]==2,"selected actor body action flag enables third option");
    words[0xa8/4]=2;shared.formation.count=3;input={};
    ui.first_update(resource,pad,input,60,&context);
    require(words[0xa4/4]==5&&words[0xa8/4]==5&&shared.hud.type2_flag_b==1,
        "third action option sets source target flag and advances without fake targets");
    words[0xa8/4]=1;words[0xd0/4]=0;shared.hud.selection_state=51;input.pressed=0x40;
    ui.first_update(resource,pad,input,60,&context);
    require(words[0xa4/4]==0xffffffff&&words[0xa8/4]==1,"shared countdown also inhibits submenu input");
}
void attack_commit_input() {
    const okage::BattleUiData data(fixture());
    std::vector<std::uint8_t> empty_ruw(48);put(empty_ruw,12,1);put(empty_ruw,16,6);
    auto resource=std::make_shared<okage::RuwResource>(empty_ruw);
    okage::BattleUiShared shared;
    okage::BattleInterface ui(shared,data,std::make_shared<int>(1),0,[](unsigned){});
    auto& words=const_cast<std::array<std::uint32_t,0xef0/4>&>(ui.words());
    const auto xdb=[](bool unit) {
        const unsigned columns=unit?27:15;
        std::vector<std::uint8_t> b(48+columns*4+10);
        put(b,0,0x424458);put(b,4,columns*4);put(b,8,10);put(b,12,1);put(b,16,0xffffffff);
        put(b,40,columns);put(b,48,5);put(b,48+(unit?1:14)*4,5);
        if(unit)put(b,56,40);
        const std::string name=unit?"unit":"item";
        std::copy(name.begin(),name.end(),b.begin()+48+columns*4);
        const std::string text="test";std::copy(text.begin(),text.end(),b.begin()+53+columns*4);
        return b;
    };
    const okage::GameDataTables tables(xdb(true),xdb(false));
    std::vector<std::uint8_t> variables(0xd78);
    for(unsigned slot:{0u,2u}) {
        put(variables,0x570+slot*4,3);const auto at=0x5c4+slot*0x50;
        put(variables,at,0x10001);put(variables,at+4,1);put(variables,at+8,32);
        variables[at+0x26]=100;
    }
    okage::UiPartySelection party;party.refresh(variables,3);
    std::array<okage::BattleActor,3> actors;
    std::array<okage::BattleEntityBinding,3> entities{};
    for(unsigned i:{0u,2u}){entities[i].actor=&actors[i];actors[i].storage[0x21c]=2;actors[i].storage[0x220]=2;}
    std::vector<unsigned> sounds;unsigned draws=0;
    okage::BattleUiFirstContext context{3,0,entities,variables,&tables,&party,
        [&](unsigned id){sounds.push_back(id);},[&]{++draws;return 5u;}};
    okage::BattleUiPad pad;words[0xe4c/4]=3;words[0xa8/4]=5;
    words[0xd4/4]=1;shared.formation.groups[0]=7;shared.formation.groups[1]=9;shared.target_count=2;
    ui.first_update(resource,pad,{},60,&context);
    require(shared.pending_action==std::array<std::uint32_t,3>{0,9,0}&&words[0xe4c/4]==3,
        "Attack prepares real group action on neutral callback without committing");
    require(variables[0x5c4+0x18]==40&&variables[0x5c4+0x26]==100&&draws==0,
        "target preparation refreshes exactly one character without consuming normal-target RNG");
    variables[0x5c5]=1;words[0xd4/4]=0;
    ui.first_update(resource,pad,{},60,&context);ui.first_update(resource,pad,{},60,&context);
    require(draws==2&&shared.pending_action[1]==9,"status1 targeting consumes live RNG on every neutral callback");
    shared.target_count=0;const auto pending_before=shared.pending_action;bool divisor_guard=false;
    try{ui.first_update(resource,pad,{},60,&context);}catch(const std::runtime_error&){divisor_guard=true;}
    require(divisor_guard&&draws==3&&shared.pending_action==pending_before,
        "zero target divisor preserves the source RNG draw before explicit rejection");
    shared.target_count=2;
    const auto before=shared.pending_action;okage::BattleUiPadInput input;input.pressed=0x20;
    ui.first_update(resource,pad,input,60,&context);
    require(words[0xa8/4]==2&&draws==3&&sounds.empty()&&shared.pending_action==before,
        "arg5 cancellation returns before refresh/RNG/action preparation and generic cancel sound");
    words[0xa8/4]=5;words[0xd0/4]=1;variables[0x5c5]=0;
    //Row5 stays the earlier cached membership; charge comparison is signed.
    variables[0x5c4+2*0x50+0x26]=0xf6;variables[0x5c4+2*0x50+0x27]=0xff;
    input={};input.pressed=0x40;
    ui.first_update(resource,pad,input,60,&context);
    require(shared.pending_action==std::array<std::uint32_t,3>{1,7,0}&&words[0xe4c/4]==5,
        "combined Attack publishes action triple and enters real fade state");
    require(variables[0x5c4+0x26]==101&&variables[0x5c4+2*0x50+0x26]==100&&variables[0x5c4+8]==32,
        "commit writes charge101 and cached participating charges100 without changing HP");
    require(words[0xa4/4]==0xffffffff&&words[0xa8/4]==5&&words[0xe8/4]==0&&sounds==std::vector<unsigned>{3},
        "final commit does not emit internal next20 as a parent command");
    shared.registry[0]->set_opacity_tween(0,128,0,128);input={};
    ui.first_update(resource,pad,input,60,&context);
    require(ui.window(0).opacity()==128&&ui.window(0).word(0x27c)==std::bit_cast<std::uint32_t>(-16.f),
        "state5 requests source fade without directly advancing opacity");
    for(unsigned i=0;i<8;++i)shared.registry[0]->advance_clock();
    ui.first_update(resource,pad,input,60,&context);
    require(words[0xa4/4]==4&&words[0xa8/4]==4&&words[0xe4c/4]==6,"zero opacity publishes actual action handoff command");
    ui.first_update(resource,pad,input,60,&context);
    require(words[0xa4/4]==0xffffffff&&words[0xa8/4]==4,"state6 clears only the one-shot command");
}
void open_and_fade_layers() {
    auto b=fixture();const okage::BattleUiData data(b);
    okage::BattleUiShared shared;
    okage::BattleInterface ui(shared,data,std::make_shared<int>(1),0,[](unsigned){});
    auto& words=const_cast<std::array<std::uint32_t,0xef0/4>&>(ui.words());
    words[0xa4/4]=0x101;words[0xa8/4]=0x102;
    words[0x184/4]=0x201;words[0x188/4]=0x202;
    words[0xe4c/4]=71;words[0xe50/4]=72;
    ui.open_layer(1);
    require(words[0x184/4]==0&&words[0x188/4]==0,
            "Open clears selected layer command and argument words");
    require(words[0xe50/4]==1&&words[0xe4c/4]==71,
            "Open writes only selected manager state via layer state index");
    require(words[0xa4/4]==0x101&&words[0xa8/4]==0x102,
            "Open preserves neighboring layer command words");
    require(ui.window(1).opacity()==1.f&&ui.window(1).word(0x27c)==0x41800000&&
            ui.window(1).word(0x274)==0x43000000&&ui.window(1).word(0x278)==0,
            "Open sets source opacity tween16 from1 with bounds0..128");
    const auto opacity0=ui.window(0).word(0x270);

    words[0x184/4]=0x211;words[0x188/4]=0x212;words[0xe50/4]=73;
    ui.reveal_layer(1,8);
    require(words[0x184/4]==0x211&&words[0x188/4]==0x212&&words[0xe50/4]==73,
            "Reveal remains opacity-only and leaves command/state words intact");
    require(ui.window(1).word(0x27c)==0x41000000&&ui.window(0).word(0x270)==opacity0,
            "Reveal applies its increment only to the selected layer");

    words[0xe4c/4]=74;
    ui.fade_layer(0,-7);
    require(ui.window(0).word(0x27c)==std::bit_cast<std::uint32_t>(-7.f)&&
            ui.window(0).opacity()==128.f&&ui.window(0).word(0x274)==0x43000000&&
            ui.window(0).word(0x278)==0,
            "Fade starts at opacity128 using source increment and bounds");
    require(words[0xe4c/4]==74&&words[0x184/4]==0x211&&ui.window(1).opacity()==1.f,
            "Fade preserves manager state and the other layer");

    const auto before=words;const auto increment0=ui.window(0).word(0x27c);
    for(auto invalid:{-16777217,16777217}) {
        bool rejected=false;
        try{ui.fade_layer(0,invalid);}catch(const std::runtime_error&){rejected=true;}
        require(rejected&&words==before&&ui.window(0).word(0x27c)==increment0,
                "Unsupported fade conversion rejects before mutation");
    }
    bool invalid_layer=false;
    try{ui.open_layer(2);}catch(const std::runtime_error&){invalid_layer=true;}
    require(invalid_layer&&words==before,"Invalid open layer rejects before mutation");
    invalid_layer=false;
    try{ui.fade_layer(2,4);}catch(const std::runtime_error&){invalid_layer=true;}
    require(invalid_layer&&words==before,"Invalid fade layer rejects before mutation");

    words[0xe90/4+1]=16;const auto invalid_state_words=words;
    const auto opacity1=ui.window(1).word(0x270);
    bool invalid_state=false;
    try{ui.open_layer(1);}catch(const std::runtime_error&){invalid_state=true;}
    require(invalid_state&&words==invalid_state_words&&ui.window(1).word(0x270)==opacity1,
            "Invalid selected state index rejects before command or opacity mutation");

    words[0xe90/4+1]=1;
    const int handle=ui.handles()[1];shared.registry[unsigned(handle)].reset();
    const auto invalid_window_words=words;
    bool invalid_owner=false;
    try{ui.open_layer(1);}catch(const std::runtime_error&){invalid_owner=true;}
    require(invalid_owner&&words==invalid_window_words,
            "Missing typed window owner rejects open before manager mutation");
}
}
int main(int argc,char** argv) {
    try {
        if(argc==4&&std::string_view(argv[1])=="--window-diagnostic") {
            std::ifstream in(argv[2],std::ios::binary);require(bool(in),"missing owned interface");
            std::vector<std::uint8_t> b((std::istreambuf_iterator<char>(in)),{});
            const okage::BattleUiData data(b);
            const std::filesystem::path path(argv[3]);
            require(!std::filesystem::exists(path)&&!std::filesystem::is_symlink(path),"refusing existing window report");
            std::ofstream out(path);require(bool(out),"cannot create window report");
            out<<"{\"schema\":\"okage-ui-window-diagnostic-v1\",\"scope\":\"198e38 and window configuration only; empty incoming streams and atlas, slots0/1; no layer constructors\",\"windows\":[";
            for(unsigned i=0;i<2;++i) {
                const auto& d=data.layers()[i];
                auto stream=std::make_shared<okage::UiWindowStream>(okage::UiWindowStream{d.stream(),{}});
                okage::UiWindow window(d.glyph_capacity(),d.sprite_capacity(),i,stream);
                const std::array<okage::UiWindow::AtlasTuple,1> atlas{};
                require(!window.configure(d.words,atlas,std::make_shared<int>(1)),"empty atlas unexpectedly dirty");
                if(i)out<<',';out<<"{\"slot\":"<<i<<",\"words\":[";
                for(unsigned at=0;at<okage::UiWindow::scalar_bytes;at+=4) {
                    if(at)out<<',';const auto value=window.known_word(at);
                    if(value)out<<*value;else out<<"null";
                }
                out<<"]}";
            }
            out<<"]}\n";require(bool(out),"cannot write window report");
            std::cout<<"owned interface window diagnostic passed\n";return 0;
        }
        if(argc==3&&std::string_view(argv[1])=="--diagnostic") {
            std::ifstream in(argv[2],std::ios::binary);require(bool(in),"missing owned interface");
            std::vector<std::uint8_t> b((std::istreambuf_iterator<char>(in)),{});
            const okage::BattleUiData data(b);
            require(data.layers()[0].glyph_capacity()==29&&data.layers()[1].sprite_capacity()==8,"owned capacities changed");
            std::cout<<"owned battle interface parsed\n";return 0;
        }
        require(argc==1,"usage: battle-ui-tests [--diagnostic owned-battle-ui.bin | --window-diagnostic owned-battle-ui.bin fresh.json]");
        first_dispatch();open_and_fade_layers();opening_selection();category_input();attack_submenu_input();attack_commit_input();
        auto b=fixture();const okage::BattleUiData data(b);b.clear();
        require(data.layers()[0].words[4]==0x80000000&&data.layers()[1].words[16]==0x80808080,"owned literal bits changed");
        require(data.layers()[0].stream()==1&&data.layers()[1].type()==3,"host bindings changed");
        okage::BattleUiShared shared;std::vector<unsigned> constructors;
        auto texture=std::make_shared<int>(1);
        {
            okage::BattleInterface ui(shared,data,texture,0x98000000,[&](unsigned type){
                constructors.push_back(type);
                require(shared.current_stream==type-1&&shared.stream_cursor==4,"source constructor stream order");
                require(shared.registry[type-2]&&shared.registry[type-2]->word(0x1960)==0,"window scans before stream replacement");
            });
            require(constructors==std::vector<unsigned>{2,3},"authored constructor order");
            require(ui.handles()==std::array<int,2>{0,1},"registry handles");
            const auto& w=ui.words();
            require(w[0x1c/4]==0x18000000&&w[0xe40/4]==2&&w[0xe44/4]==3&&w[0xe94/4]==1,"manager scalar fields");
            for(unsigned at:{0x88u,0x8cu,0x168u,0x16cu,0xed8u})require(w[at/4]==0xffffffff,"manager postwrites overlap layers");
            require(w[0x114/4]==0&&w[0x1f4/4]==0,"layer manager link remains null before callbacks");
        }
        require(!shared.registry[0]&&!shared.registry[1],"registry releases owned windows");
        {
            shared.atlas={okage::UiWindow::AtlasTuple{1,2,3,4},okage::UiWindow::AtlasTuple{}};
            okage::BattleInterface ui(shared,data,texture,0,[](unsigned){});
            require(ui.window(0).word(0x1960)==2&&ui.window(1).word(0x1960)==2,"reentry scans persistent streams");
            require(shared.atlas_dirty&&ui.window(0).word(0xc)==1,"actual runtime atlas owner used");
        }
        for(unsigned n:{0u,7u,24u,87u,88u,311u,313u}) {b=fixture();b.resize(n);rejects(b);}
        for(unsigned at:{0u,8u,12u,16u,20u,24u,56u,88u,88u+84u,88u+88u,88u+92u,88u+96u}) {
            b=fixture();put(b,at,0xdeadbeef);rejects(b);
        }
        for(unsigned i=0;i<2;++i)for(auto [offset,value]:{
            std::pair{4u,39u}, {8u,0u}, {12u,0u}, {80u,2u}, {100u,0u},
            {100u,33u}, {104u,0xffffffffu}, {108u,1u}, {16u,0x7f800000u}, {60u,0x7fc00000u}}) {
            b=fixture();put(b,88+i*112+offset,value);rejects(b);
        }
        std::vector<std::uint8_t> vars(0x1000);okage::initialize_game_variables(vars);
        auto character=[&](unsigned slot,unsigned member,unsigned id,unsigned hp,unsigned charge) {
            put(vars,0x570+slot*4,member);const unsigned at=0x5c4+slot*0x50;
            put(vars,at+0, id<<16);put(vars,at+8,hp);
            vars[at+0x26]=charge;vars[at+0x27]=0;
        };
        character(0,3,10,99,100);character(1,1,11,50,100);
        character(2,3,12,0,100);character(3,3,13,80,99);
        character(4,3,14,1,100);character(6,3,16,100,100);
        okage::UiPartySelection party;party.refresh(vars,3);
        require(party.counts()==std::array<std::uint32_t,8>{5,5,4,4,2,2,2,2},"party row counts");
        require(party.at(0,1)==11&&party.at(1,1)==1,"owned roster rows");
        require(party.at(2,1)==12&&party.at(3,1)==2,"active party rows");
        require(party.at(4,1)==14&&party.at(5,1)==4&&party.at(6,1)==3&&party.at(7,1)==4,"ready party ordinal differs from slot");
        require(party.at(2,4)==-1&&party.at(6,2)==-1,"unused selection sentinel");
        const auto unchanged=vars;party.refresh(vars,3);require(vars==unchanged,"non-demo refresh mutated variables");
        okage::set_event_variable(vars,5,4,1);party.refresh(vars,3);
        require(party.counts()==std::array<std::uint32_t,8>{1,1,1,1,1,1,1,1},"restricted party selection");
        okage::set_event_variable(vars,5,4,0);party.refresh(vars,4);
        require(party.at(5,1)==3&&party.at(6,1)==2&&party.counts()[5]==3,"demo charge refresh");
        require(vars[0x5c4+3*0x50+0x26]==100,"demo writes actual shared charge");
        character(1,0xffffffff,0xffff,0xffffffff,100);
        character(4,3,0xffff,0xffffffff,100);party.refresh(vars,3);
        require(party.counts()[0]==4&&party.at(0,3)==-1,"signed membership and ID");
        require(party.at(5,2)==4,"negative readiness word is nonzero");
        character(3,3,13,80,9);character(6,3,16,100,7);
        okage::set_event_variable(vars,5,4,1);party.refresh(vars,4);
        require(vars[0x5c4+3*0x50+0x26]==9&&vars[0x5c4+6*0x50+0x26]==7,"demo must preserve skipped slots");
        std::cout<<"battle UI data/party tests passed\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
