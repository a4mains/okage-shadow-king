#include "okage/input_log.hpp"
#include "restore_session.hpp"
#include "results_renderer.hpp"
#include "okage/restore_data.hpp"
#include "okage/save_store.hpp"
#include "okage/results_text.hpp"
#include "okage/load_queue.hpp"
#include <algorithm>
#include <bit>
#include <cfenv>
#include <fstream>
#include <iostream>
#include <stdexcept>
#pragma STDC FENV_ACCESS ON
#pragma STDC FP_CONTRACT OFF
namespace okage {
namespace {
using Text=RestoreStringId;
constexpr int keep=RuwResource::preserve_component;
void check(bool value,const char* why){if(!value)throw std::runtime_error(why);}
std::vector<std::uint8_t> read(const std::filesystem::path& path) {
    const auto n=std::filesystem::file_size(path);check(n<=64*1024*1024,"Restore asset extent");
    std::vector<std::uint8_t> b(n);std::ifstream in(path,std::ios::binary);
    check(bool(in.read(reinterpret_cast<char*>(b.data()),n)),"Restore asset read");record_input_read(path,b);return b;
}
std::uint32_t word(Bytes b,unsigned at){check(at+4<=b.size(),"Restore payload word");return unsigned(b[at])|unsigned(b[at+1])<<8|unsigned(b[at+2])<<16|unsigned(b[at+3])<<24;}
int signed_word(Bytes b,unsigned at){return std::bit_cast<std::int32_t>(word(b,at));}
unsigned half(Bytes b,unsigned at){check(at+2<=b.size(),"Restore payload halfword");return unsigned(b[at])|unsigned(b[at+1])<<8;}
float hp_fraction(int current,int maximum) {
    const int prior=std::fegetround();check(prior>=0&&!std::fesetround(FE_TOWARDZERO),"Restore HP rounding mode");
    volatile float numerator=float(current),denominator=float(maximum),result=numerator/denominator;
    std::fesetround(prior);return result;
}
std::string string_field(Bytes b,unsigned at,unsigned end) {
    check(at<end&&end<=b.size(),"Restore string field extent");
    const auto stop=std::find(b.begin()+at,b.begin()+end,0);check(stop!=b.begin()+end,"Restore string field terminator");
    return {b.begin()+at,stop};
}
std::string format(std::string_view source,std::initializer_list<std::string> values) {
    auto arg=values.begin();std::string out;
    for(std::size_t i=0;i<source.size();) {
        if(source[i]!='%'){out+=source[i++];continue;}
        ++i;check(i<source.size(),"Restore text conversion");
        const bool zero=source[i]=='0';unsigned width=0;
        while(i<source.size()&&source[i]>='0'&&source[i]<='9'){width=width*10+source[i++]-'0';check(width<128,"Restore text width");}
        check(i<source.size()&&(source[i]=='s'||source[i]=='d')&&arg!=values.end(),"Restore text arguments");
        if(zero&&width>arg->size()&&!arg->empty()&&(*arg)[0]=='-') {
            out+='-';out.append(width-arg->size(),'0');out+=arg->substr(1);
        } else {
            if(width>arg->size())out.append(width-arg->size(),zero?'0':' ');
            out+=*arg;
        }
        ++arg;++i;
    }
    check(arg==values.end()&&out.size()<128,"Restore text length/arguments");return out;
}
}
struct RestoreSession::Impl {
    enum Io : unsigned {None,Checking,Scanning,Ready,Reading,Loaded,Failed,Writing,Written};
    bool live=true,manager=true,dead=false,completed=false,save=false;
    unsigned phase=0,confirmation=0,success_ticks=0;
    Io io=None;
    RestoreShared& shared;BattleUiShared& ui;
    std::array<std::uint32_t,0xef0/4> words{};
    int handle=-1;
    StageSettings settings;ShadowScreenParameters ui_projection=initial_ui_projection();
    std::shared_ptr<const std::vector<Font>> fonts;
    RestoreUiData data;ResultsTextData numbers;
    std::shared_ptr<RuwResource> ruw;
    std::shared_ptr<const Texture> atlas;
    std::unique_ptr<ResultsRenderer> renderer;
    NativeSaveStore store;
    std::filesystem::path save_directory;
    std::array<std::optional<SavePayload>,3> slots;
    std::array<std::string,3> slot_errors;
    std::array<std::optional<SavePayload>,3> pending_slots;
    std::array<std::string,3> pending_errors;
    std::optional<SavePayload> loaded;
    std::string load_error,status;
    const Texture* output=nullptr;
    NativeLoadQueue jobs;
    Impl(SceneClearColor& clear,SceneFade& fade,BattleUiShared& u,RestoreShared& s,RestoreConstruction c)
        :shared(s),ui(u),fonts(std::move(c.fonts)),data(read(c.ui_root/"RESTORE.bin")),
         numbers(read(c.ui_root/"results-text.bin")),store(c.save_directory),save_directory(c.save_directory) {
        save=c.save;
        if(save) {
            check(c.field_ruw&&c.field_atlas,"Save requires live field UI resources");
            ruw=std::move(c.field_ruw);atlas=std::move(c.field_atlas);
        } else {
            clear.rgb={0,0,0};fade.color=0;initialize_scene_ui(ui);fade.immediate(0);
            check(bool(c.stop_music),"Restore music owner");c.stop_music();
            atlas=std::make_shared<const Texture>(decode_xim(read(c.common_root/"2D/MAIN.XPF/bm_if.xim")));
            ruw=std::make_shared<RuwResource>(read(c.ui_root/"FLD.RUW"));
        }
        renderer=std::make_unique<ResultsRenderer>(fonts,atlas);
        const auto free=std::find(ui.registry.begin(),ui.registry.end(),nullptr);
        check(free!=ui.registry.end(),"Restore UI registry exhausted");handle=int(free-ui.registry.begin());
        auto stream=std::make_shared<UiWindowStream>(UiWindowStream{1,{
            UiStreamCommand{14,UiStreamReference::MenuTransform1f9c00},UiStreamCommand{6,UiStreamReference::MenuTransform1f9c2c}}});
        const auto& d=data.definition;const auto& row=d.words;
        try {
            *free=std::make_shared<UiWindow>(d.glyph_capacity(),d.sprite_capacity(),handle,stream);
            if((*free)->configure(row,data.atlas,atlas))ui.resource.layout_shared().sprite_dirty=true;
            ui.atlas_texture=-1;ui.current_stream=1;ui.stream_cursor=4;
            constexpr unsigned base=0x40/4;
            words[base]=handle;words[base+1]=1;words[base+0x40/4]=row[1];words[base+0x44/4]=row[2];
            std::copy_n(row.begin()+4,12,words.begin()+base+4);
            words[base+0x4c/4]=0xffffffff;words[base+0x54/4]=1;
            for(unsigned i=0;i<8;++i){words[base+0x6c/4+i]=0xffffffff;words[base+0xac/4+i]=0xffffffff;}
            words[base+0x64/4]=0xffffffff;
            words[base+0x50/4]=0x50;words[base+0x5c/4]=1;words[base+0x60/4]=8;
            words[0xe40/4]=1;
            words[0x8c/4]=words[0xed8/4]=words[0x168/4]=words[0x16c/4]=words[0x88/4]=0xffffffff;
            words[0xe44/4]=words[0x180/4];
            for(int i=0;i<3;++i)ruw->set_text(8,0x6b+i,data.string(Text::TextPrefix));
            if(!save)fade.immediate(1);
        }catch(...){destroy_window();throw;}
    }
    ~Impl(){destroy_window();}
    void destroy_window(){if(handle>=0){ui.registry[unsigned(handle)].reset();handle=-1;}}
    UiWindow& window(){check(handle>=0&&bool(ui.registry[unsigned(handle)]),"Restore window lifetime");return *ui.registry[unsigned(handle)];}
    std::string text(Text id,std::initializer_list<std::string> args={}) const{return format(data.string(id),args);}
    void bind() {ui.resource_accumulators[0]=0;const std::array<std::size_t,1> groups{8};window().bind_ruw(ruw,groups);}
    void close(const std::function<void(unsigned)>& sound) {shared.state=-1;window().set_opacity_tween(-4,128,0,128);sound(4);}
    void first(BattleUiPadInput input,const std::function<void(unsigned)>& sound) {
        words[0xe8c/4]=0;words[0xe48/4]=0;words[0x114/4]=1;
        bool up=false,down=false,left=false,right=false;
        const int x=int(input.left_x)-128,y=int(input.left_y)-128;
        if(shared.repeat_delay==0) {
            up=y< -100||(input.repeat&0x1000);down=y>100||(input.repeat&0x4000);
            left=x< -100||(input.repeat&0x8000);right=x>100||(input.repeat&0x2000);
            if(up||down||left||right)shared.repeat_delay=8;
        } else {--shared.repeat_delay;if(x>=-99&&x<100&&y>=-99&&y<100)shared.repeat_delay=0;}
        if(window().opacity()==0)shared.state=-2;
        if(shared.state==0)completed=false;
        ruw->set_text(8,7,data.string(Text::Empty));ruw->set_position(8,0x10,-193,keep); //11b7c0
        if(shared.state>0&&shared.previous_opacity!=window().opacity()) {
            shared.previous_opacity=window().opacity();++words[0xe8c/4];return;
        }
        const bool buttons_centered=x>=-99&&x<=99&&y>=-99&&y<=99&&(input.held==0x20||input.held==0x40);
        switch(shared.state) {
        case 0:
            window().reset_for_open();window().set_opacity_tween(8,1,0,128);bind();shared.state=save?10:510;if(!save)confirmation=0;break;
        case 510:case 540:case 10:
            if(input.pressed&0x20)close(sound);break;
        case 50:case 550:
            check(save==(shared.state==50),"Save/Restore selection state owner");
            status=text(save?Text::SaveSlotPrompt:Text::ReadSlotPrompt);ruw->set_text(8,7,data.string(Text::SlotLabels));
            ruw->set_position(8,7,47,keep);ruw->set_position(8,0x10,shared.selected*75+43,keep);words[0xa4/4]=0xffffffff;
            for(int i=0;i<3;++i)ruw->set_secondary_rgba(8,0x12+i*22,i==shared.selected?std::array<int,4>{230,160,40,90}:std::array<int,4>{40,45,190,90});
            //Save selection changes precede confirm; Restore does the reverse.
            if(save) {
                if(up||left){sound(7);shared.selected=(shared.selected+2)%3;}
                if(down||right){sound(7);shared.selected=(shared.selected+1)%3;}
            }
            if(buttons_centered) {
                if(save) {
                    if(input.pressed&0x40){shared.state=60;confirmation=1;sound(3);}
                    if(input.pressed&0x20)close(sound);
                } else {
                    if(input.pressed&0x20)close(sound);
                    if((input.pressed&0x40)&&slots[shared.selected]){shared.state=560;confirmation=1;sound(3);}
                }
            }
            if(!save) {
                if(up||left){sound(7);shared.selected=(shared.selected+2)%3;}
                if(down||right){sound(7);shared.selected=(shared.selected+1)%3;}
            }break;
        case 70:case 560:
            ruw->set_text(8,7,data.string(Text::YesNo));ruw->set_position(8,7,61,keep);ruw->set_position(8,0x10,int(confirmation)*65+43,keep);
            if(left){confirmation=0;sound(7);}if(right){confirmation=1;sound(7);}
            if(buttons_centered) {
                if(input.pressed&0x20){shared.state=save?50:550;sound(4);}
                if(input.pressed&0x40){shared.state=confirmation?(save?50:550):(save?100:700);sound(confirmation?4:3);}
            }break;
        case 130:case 710:case 720:
            if(input.pressed&0x60){shared.state=10;sound(4);}break;
        case 151:case 999:shared.state=-1;window().set_opacity_tween(-4,128,0,128);break;
        case -1:break;
        case -2:
            shared.state=0;dead=true;words[0x1c/4]|=0x80000000u;break;
        case 30:case 40:case 60:case 100:case 530:case 700:break;
        default:throw std::runtime_error("Restore input state unrecovered: "+std::to_string(shared.state));
        }
        ++words[0xe8c/4];
    }
    void empty_slots() {
        confirmation=1;
        ruw->set_position(8,0x5c,56,101);ruw->set_position(8,0x5e,51,194);ruw->set_position(8,0x5d,49,291);
        for(int i=0;i<3;++i) {
            const int b=i*22;ruw->set_secondary_rgba(8,b+0x12,{40,45,190,90});
            ruw->set_text(8,i+0x56,text(Text::SlotNumberFormat,{std::to_string(i+1)}));
            const float scale=std::bit_cast<float>(0x4079999au);ruw->set_runtime_float_pair(8,i+0x56,scale,scale);
            for(int id:{0x14,0x15,0x16,0x17,0x18,0x19,0x1a})ruw->set_text(8,b+id,data.string(Text::Empty));
            for(int id:{0x1b,0x1c,0x1d,0x26,0x1e,0x1f,0x23,0x20,0x24,0x21,0x25,0x22})ruw->set_rgba(8,b+id,{0,0,0,keep});
            ruw->set_fill_fraction(8,b+0x27,0);ruw->set_position(8,b+0x27,650,0);
            const float small=std::bit_cast<float>(0x3f733333u);
            for(int id:{0x18,0x19,0x1a})ruw->set_runtime_float_pair(8,b+id,small,small);
        }
    }
    void slot_layout(unsigned slot,const GameDataTables& tables) {
        const Bytes bytes=*slots.at(slot);const int b=int(slot)*22;
        ruw->set_text(8,slot+0x6b,text(Text::NameFormat,{string_field(bytes,0x864,0x878)}));
        for(int i=0;i<2;++i){ruw->set_runtime_word0(8,slot*2+0x5f+i,std::uint32_t(-58));ruw->set_rgba(8,slot*2+0x5f+i,{128,128,128,keep});}
        int icon=slot*2+0x5f;
        for(unsigned i=1;i<6;++i)if(word(bytes,0x570+i*4)==3&&word(bytes,0x524)==0){ruw->set_runtime_word0(8,icon,i+0x38);ruw->set_rgba(8,icon++,{255,255,255,keep});}
        const auto unit=std::bit_cast<std::int16_t>(std::uint16_t(half(bytes,0x5c6)));
        check(unit>=0,"Save preview unit ID is negative");
        (void)tables.ui_unit(unsigned(unit));
        ruw->set_rgba(8,slot+0x59,{255,255,255,keep});
        ruw->set_text(8,b+0x14,text(Text::PlayerNameFormat,{string_field(bytes,0x844,0x854)}));
        const int seconds=signed_word(bytes,0x520);int hours=seconds>0?seconds/3600:0,minutes=seconds>0?(seconds/60)%60:0;
        if(hours>99){hours=99;minutes=59;}
        ruw->set_text(8,b+0x15,text(Text::PlayTimeFormat,{std::to_string(hours),std::to_string(minutes)}));
        for(int id:{0x1d,0x26,0x1b,0x1c,0x1e,0x1f,0x23,0x20,0x24,0x25,0x22})ruw->set_rgba(8,b+id,{255,255,255,keep});
        ruw->set_runtime_word0(8,b+0x1e,0x16);
        ruw->set_text(8,b+0x16,text(Text::HitPointsFormat,{std::to_string(signed_word(bytes,0x5cc)),std::to_string(std::bit_cast<std::int16_t>(std::uint16_t(half(bytes,0x5dc))))}));
        const auto maximum=std::bit_cast<std::int16_t>(std::uint16_t(half(bytes,0x5dc)));check(maximum!=0,"Restore saved HP maximum is zero");
        ruw->set_fill_fraction(8,b+0x27,hp_fraction(signed_word(bytes,0x5cc),maximum));ruw->set_position(8,b+0x27,253,slot*94+110);
        ruw->set_text(8,b+0x17,text(Text::LevelFormat,{std::to_string(std::bit_cast<std::int16_t>(std::uint16_t(half(bytes,0x5c8))))}));
        ruw->set_text(8,b+0x18,data.string(Text::NeutralStatus));
        const int status_id=std::bit_cast<std::int8_t>(bytes[0x5c4])+0x38,curse=std::bit_cast<std::int8_t>(bytes[0x5c5]);
        check(status_id>=0&&curse+0x2f>=0,"Restore saved status message index");
        ruw->set_text(8,b+0x19,text(Text::GenericStringFormat,{tables.system_message(status_id)}));
        ruw->set_rgba(8,b+0x21,{195,191,215,keep});
        ruw->set_text(8,b+0x1a,text(Text::CurseStatusFormat,{tables.system_message(curse+0x2f)}));
        ruw->set_runtime_word0(8,b+0x22,curse==0?0x5c:curse+0x52);
        if(!curse)ruw->set_rgba(8,b+0x22,{195,191,215,keep});
    }
    void second(std::span<std::uint8_t> variables,const GameDataTables& tables) {
        //11c2c8 updates display fields before advancing its file coroutine.
        for(int i=0;i<3;++i) {
            shared.alpha[i]=std::clamp(shared.alpha[i]+(shared.selected==i?4:-4),30,128);
            ruw->set_class_rgba(8,i+10,{keep,keep,keep,0});ruw->add_class_rgba(8,i+10,{keep,keep,keep,shared.alpha[i]});
            ruw->set_class_rgba(8,i,{keep,keep,keep,0});if(slots[i])ruw->add_class_rgba(8,i,{keep,keep,keep,shared.alpha[i]});
            ruw->set_text(8,i+0x68,text(Text::MenuNumberFormat,{std::to_string(i+1)}));
        }
        const auto& selected=slots.at(shared.selected);
        const Bytes summary=save?Bytes(variables):selected?Bytes(*selected):Bytes{};
        ruw->set_text(8,4,text(Text::ProgressNumberFormat,{std::to_string(summary.empty()?0:std::min(signed_word(summary,0x590),99))}));
        ruw->set_text(8,5,numbers.money_summary(summary.empty()?0:std::max(signed_word(summary,0x51c),0)));
        ruw->set_text(8,6,status);
        if(save&&io==Failed&&shared.state==10) {
            slots={};slot_errors={};pending_slots={};pending_errors={};loaded.reset();load_error.clear();io=None;
        }
        if(io==None) {
            empty_slots();status=text(Text::CheckingCardNotice);
            jobs.enqueue([this]{
                if(save)std::filesystem::create_directories(save_directory);
                check(std::filesystem::is_directory(save_directory),"Restore storage directory unavailable");
                check(std::filesystem::space(save_directory).available!=std::uintmax_t(-1),"Restore storage capacity unavailable");
            },{0x16d580,0x11c9b8});
            jobs.resume();io=Checking;
        }
        if(io==Checking) {
            jobs.check();if(jobs.pending()||dead||shared.state<0)return;
            shared.state=save?30:530;
            jobs.enqueue([this]{for(unsigned i=0;i<3;++i){try{pending_slots[i]=store.read(i);}catch(const std::exception& e){pending_errors[i]=e.what();}}},{0x16cdb8,0x11c2c8});
            jobs.resume();io=Scanning;
        }
        if(io==Scanning) {
            jobs.check();if(jobs.pending())return;
            slots=std::move(pending_slots);slot_errors=std::move(pending_errors);
            bool any=false;
            for(unsigned i=0;i<3;++i){if(!slot_errors[i].empty())std::cerr<<"Native Restore slot "<<i<<": "<<slot_errors[i]<<'\n';if(slots[i]){slot_layout(i,tables);any=true;}}
            if(!dead&&shared.state>=0)shared.state=save?50:any?550:540;
            status=save?text(Text::SaveSlotPrompt):any?text(Text::ReadSlotPrompt):text(Text::NoSavedDataNotice);io=Ready;
        }
        if(save) {
            if(io==Ready&&shared.state==60) {
                check(std::filesystem::space(save_directory).available>=source_save_file_size,"Save storage has insufficient free space");
                if(slots[shared.selected]) {shared.state=70;status=text(Text::ReplaceSavePrompt,{std::to_string(shared.selected+1)});}
                else shared.state=100;
            }
            if(io==Ready&&shared.state==100) {
                check(variables.size()>=save_payload_size,"Save variable payload extent");
                SavePayload snapshot;std::copy_n(variables.begin(),snapshot.size(),snapshot.begin());
                const unsigned slot=shared.selected;status=text(Text::SavingNotice);load_error.clear();loaded.reset();
                jobs.enqueue([this,slot,snapshot]{try{store.write(slot,snapshot);}catch(const std::exception& e){load_error=e.what();}},{0x16cfb0,0x11d904});
                jobs.resume();io=Writing;
            }
            if(io==Writing) {
                jobs.check();if(jobs.pending())return;
                if(!load_error.empty()){shared.state=130;status=text(Text::SaveFailedNotice);io=Failed;return;}
                const unsigned slot=shared.selected;
                jobs.enqueue([this,slot]{try{loaded=store.read(slot);if(!loaded)load_error="Written native save disappeared before readback";}catch(const std::exception& e){load_error=e.what();}},{0x16d788,0x11daf0});
                jobs.resume();io=Written;
            }
            if(io==Written) {
                jobs.check();if(jobs.pending())return;
                check(load_error.empty()&&loaded,"Save readback failure remains unrecovered");
                slots[shared.selected]=*loaded;slot_layout(shared.selected,tables);
                success_ticks=0;status=text(Text::SaveCompleteNotice);io=Loaded;return;
            }
            if(io==Loaded&&shared.state==100) {if(++success_ticks>=180){completed=true;shared.state=151;}}
            return;
        }
        if(io==Ready&&shared.state==560)status=text(Text::ReadFilePrompt,{std::to_string(shared.selected+1)});
        if(io==Ready&&shared.state==700) {
            const unsigned slot=shared.selected;
            jobs.enqueue([this,slot]{try{loaded=store.read(slot);if(!loaded)load_error="Native save slot disappeared during Restore";}catch(const std::exception& e){load_error=e.what();}},{0x16cdb8,0x11e1cc});
            jobs.resume();io=Reading;
        }
        if(io==Reading) {
            jobs.check();if(jobs.pending())return;
            success_ticks=0;
            if(load_error.empty()&&loaded) {
                check(variables.size()>=loaded->size(),"Restore destination variable extent");
                std::copy(loaded->begin(),loaded->end(),variables.begin());status=text(Text::LoadSuccessNotice);io=Loaded;
            } else {std::cerr<<"Native Restore failed: "<<load_error<<'\n';shared.state=710;status=text(Text::LoadFailedNotice);io=Failed;}
            return;
        }
        if(io==Loaded&&shared.state==700) {if(++success_ticks>=120){completed=true;shared.state=999;}}
        if(io==Failed&&shared.state==10){throw std::runtime_error("Restore retry after failed read remains unrecovered");}
    }
};
RestoreSession::RestoreSession(SceneClearColor& c,SceneFade& f,BattleUiShared& u,RestoreShared& s,RestoreConstruction setup)
    :impl_(std::make_unique<Impl>(c,f,u,s,std::move(setup))){}
RestoreSession::~RestoreSession()=default;
void RestoreSession::tick(BattleUiPadInput input,std::span<std::uint8_t> variables,const GameDataTables& tables,
    SceneFade& fade,Animation& camera,FieldProjection& projection,RetiringSceneEffects* retiring,BattleUiShared& ui,
    unsigned& image_counter,const SineTable& sine,GameRandom& random,const std::function<void(unsigned)>& sound,const std::function<void(bool)>& finish) {
    auto& p=*impl_;check(p.live,"Dead Restore resource");const bool unlink=p.manager&&p.dead;
    if(p.phase==0){if(!p.manager||unlink)p.phase=1;}else if(p.phase==1){fade.immediate(0);finish(p.completed);}else throw std::runtime_error("Restore parent phase");
    (void)image_counter;camera.tick_channels();projection.submit_empty_resource_frame(true,p.settings.cached_depth_min,p.settings.cached_depth_max);
    if(retiring)retiring->tick();std::vector<UiResourceSlotBinding> bindings;
    if(p.manager&&!p.dead)bindings.push_back({unsigned(p.handle),0});
    const auto draw=ui.resource.advance_resource(ui.registry,bindings,ui.resource_accumulators,p.fonts,sine,random,p.ui_projection);
    p.output=&p.renderer->render(draw,fade);
    if(unlink)p.manager=false;
    else if(p.manager){p.first(input,sound);p.second(variables,tables);if(p.dead)p.destroy_window();}
}
void RestoreSession::tick_save(BattleUiPadInput input,std::span<std::uint8_t> variables,const GameDataTables& tables,
    SceneFade& fade,const Texture& background,const SineTable& sine,GameRandom& random,
    const std::function<void(unsigned)>& sound,const std::function<void(bool)>& publish_result) {
    auto& p=*impl_;check(p.live&&p.save&&p.manager&&!p.dead,"Save manager lifetime");
    const std::array<UiResourceSlotBinding,1> bindings{{{unsigned(p.handle),0}}};
    const auto draw=p.ui.resource.advance_resource(p.ui.registry,bindings,p.ui.resource_accumulators,p.fonts,sine,random,p.ui_projection);
    p.output=&p.renderer->render(draw,fade,&background);
    p.first(input,sound);
    if(p.dead)publish_result(p.completed); //11e480 publishes before11c2c8.
    p.second(variables,tables);
    if(p.dead)p.destroy_window();
}
bool RestoreSession::dead() const{return impl_->dead;}
void RestoreSession::retire(Animation& camera,FieldProjection& projection,RetiringSceneEffects* effects,BattleUiShared& ui) {
    auto& p=*impl_;check(p.live,"Restore retired twice");if(effects)effects->mark();
    p.destroy_window();for(auto& w:ui.registry)w.reset();projection.reset_scene();reset_scene_camera(camera);
    p.manager=false;p.renderer.reset();p.output=nullptr;p.live=false;
}
bool RestoreSession::resource_live() const{return impl_->live;}
const Texture* RestoreSession::rendered_image() const{return impl_->live?impl_->output:nullptr;}
std::vector<std::uint32_t> RestoreSession::observation() const {
    const auto& p=*impl_;
    std::vector<std::uint32_t> out{p.phase,unsigned(p.manager),unsigned(p.dead),unsigned(p.completed),
        std::bit_cast<std::uint32_t>(p.shared.state),unsigned(p.shared.selected),p.confirmation,p.io,p.success_ticks,
        std::bit_cast<std::uint32_t>(p.handle<0?0.f:p.ui.registry[unsigned(p.handle)]->opacity()),
        std::bit_cast<std::uint32_t>(p.shared.previous_opacity),unsigned(p.shared.repeat_delay)};
    for(unsigned i=0;i<3;++i){out.push_back(unsigned(p.shared.alpha[i]));out.push_back(unsigned(bool(p.slots[i])));out.push_back(unsigned(!p.slot_errors[i].empty()));}
    out.insert(out.end(),p.words.begin(),p.words.end());return out;
}
}
