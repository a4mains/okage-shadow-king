#include "okage/results_interface.hpp"
#include "okage/battle_results.hpp"
#include "okage/event.hpp"
#include <algorithm>
#include <bit>
#include <stdexcept>

namespace okage {
namespace {
constexpr auto keep=RuwResource::preserve_component;
constexpr unsigned base=0x40/4;
std::int32_t signed_word(std::uint32_t v){return std::bit_cast<std::int32_t>(v);}
unsigned half(Bytes b,unsigned at) {
    if(at>b.size()||b.size()-at<2)throw std::runtime_error("Results reward halfword extent");
    return unsigned(b[at])|(unsigned(b[at+1])<<8);
}
int signed_half(Bytes b,unsigned at){return std::bit_cast<std::int16_t>(std::uint16_t(half(b,at)));}
std::string substitute(std::string_view format,std::span<const std::string> arguments) {
    std::string out;unsigned argument=0;
    for(std::size_t i=0;i<format.size();) {
        if(format[i]!='%'){out+=format[i++];continue;}
        if(format.substr(i,2)!="%s"||argument>=arguments.size())
            throw std::runtime_error("Results text substitution outside source format");
        out+=arguments[argument++];i+=2;
    }
    if(argument!=arguments.size())throw std::runtime_error("Results text argument count");
    return out;
}
}
ResultsInterface::ResultsInterface(UiResourceCoordinator::Registry& registry,
    UiResourceCoordinator& coordinator,std::array<std::uint32_t,16>& accumulators,
    ResultsUiShared& shared,const ResultsUiData& data,std::shared_ptr<RuwResource> resource,
    std::shared_ptr<const void> texture,std::uint32_t parent_flags,ResultsUiContext context)
    :registry_(registry),accumulators_(accumulators),shared_(shared),data_(data),resource_(std::move(resource)) {
    if(!resource_||!texture)throw std::runtime_error("Results interface requires owned RUW and texture");
    if(context.game_mode!=2)throw std::runtime_error("Results interface supports field-return mode2");
    const auto& definition=data.definition();const auto& w=definition.words;
    const auto free=std::find(registry_.begin(),registry_.end(),nullptr);
    if(free==registry_.end())throw std::runtime_error("Results UI registry exhausted");
    const unsigned slot=free-registry_.begin();
    try {
        *free=std::make_shared<UiWindow>(definition.glyph_capacity(),definition.sprite_capacity(),slot,shared_.stream);
        handle_=int(slot);
        if((*free)->configure(w,shared_.atlas,texture))coordinator.layout_shared().sprite_dirty=true;
        words_[0x1c/4]=parent_flags&0x18000000u;
        words_[base]=slot;words_[base+1]=1;
        words_[base+0x54/4]=1;
        words_[base+0x40/4]=w[1];words_[base+0x44/4]=w[2];
        std::copy_n(w.begin()+4,12,words_.begin()+base+4);
        words_[base+0x4c/4]=0xffffffffu;
        for(unsigned i=0;i<8;++i) {
            words_[base+0x6c/4+i]=0xffffffffu;words_[base+0xac/4+i]=0xffffffffu;
        }
        words_[base+0x64/4]=0xffffffffu;
        shared_.stream->entries={UiStreamCommand{14,UiStreamReference::MenuTransform1f9c00},
                                  UiStreamCommand{6,UiStreamReference::MenuTransform1f9c2c}};
        initialize(context);
        words_[base+0x50/4]=0x50;words_[base+0x5c/4]=1;words_[base+0x60/4]=11;
        words_[0xe90/4]=0;words_[0xe40/4]=1;words_[0xe44/4]=words_[0x180/4];
        words_[0x8c/4]=0xffffffffu;words_[0xed8/4]=0xffffffffu;
        words_[0x168/4]=0xffffffffu;words_[0x16c/4]=0xffffffffu;words_[0x88/4]=0xffffffffu;
    } catch(...) {release();throw;}
}
void ResultsInterface::release(){if(handle_>=0){registry_[unsigned(handle_)].reset();handle_=-1;}}
ResultsInterface::~ResultsInterface(){release();}
unsigned ResultsInterface::handle() const {
    if(handle_<0||!registry_[unsigned(handle_)])throw std::runtime_error("Results window lifetime");
    return unsigned(handle_);
}
const UiWindow& ResultsInterface::window() const {return *registry_[handle()];}
UiWindow& ResultsInterface::mutable_window(){return *registry_[handle()];}

void ResultsInterface::initialize(ResultsUiContext c) {
    if(c.reward.size()<0x30)throw std::runtime_error("Results constructor reward extent");
    auto& s=shared_;auto& r=*resource_;
    s.progress.notice_wait=0;s.progress.notice_row=0;s.progress.notice_counters.fill(-1);
    s.progress.notice=0;
    s.animation.timer=s.animation.advance=0;s.loader_ready=s.skip=s.item_count=0;
    c.party.refresh(c.variables,c.game_mode);
    for(unsigned i=0;i<8;++i)if(signed_half(c.reward,4+i*4)>0)++s.item_count;
    for(unsigned i=0;i<8;++i) {
        const int id=signed_half(c.reward,4+i*4);
        const auto name_id=int(data_.widget_ids()[i]),quantity_id=int(data_.widget_ids()[8+i]);
        if(id<=0) {
            const auto empty=substitute(c.tables.system_message(0x91),{});
            r.set_text(13,name_id,empty);r.set_text(13,quantity_id,empty);
        } else {
            const std::array name{c.tables.item_name(id)};
            r.set_text(13,name_id,substitute(c.tables.system_message(0x104),name));
            const std::array quantity{c.numbers.number2(signed_half(c.reward,6+i*4))};
            r.set_text(13,quantity_id,substitute(c.tables.system_message(0x92),quantity));
            r.set_position(13,quantity_id,int(i/4)*219+194,int(i%4)*24+115);
            r.set_position(13,name_id,int(i/4)*219+31,int(i%4)*24+115);
        }
    }
    s.reward_display=s.money_display=0;s.money_before=std::uint32_t(event_variable(c.variables,0x40003));
    r.set_text(13,0x44,c.text.money(0));r.set_text(13,0x45,c.text.money(signed_word(s.money_before)));
    s.progress.initialize(c.variables,c.tables,c.party,c.reward,r,c.numbers,c.game_mode,c.play_jingle);
    const std::array lp{c.numbers.number2(std::min(event_variable(c.variables,0x40020),99))};
    r.set_text(13,0x3e,substitute(c.tables.system_message(0x6b),lp));
    r.set_text(13,0x3f,c.text.money_summary(signed_word(s.money_before)));
    const auto empty=substitute(c.tables.system_message(0x88),{});
    r.set_text(1,4,empty);r.set_text(1,5,empty);
}
void ResultsInterface::currency(bool quick,ResultsUiContext c) {
    auto& s=shared_;const auto reward=half(c.reward,2);
    s.reward_display+=12;
    if(signed_word(s.reward_display)>int(reward))s.reward_display=reward;
    resource_->set_text(13,0x44,c.text.money(signed_word(s.reward_display)));
    s.animation.currency(*resource_,data_,quick);
    s.money_display+=12;
    if(signed_word(s.money_display)>int(reward)) {
        s.money_display=reward;
        if(!quick){s.animation.advance=1;s.animation.timer=0;}
    }
    resource_->set_text(13,0x45,c.text.money(signed_word(s.money_before+s.money_display)));
}
void ResultsInterface::party_alpha(bool quick,ResultsUiContext c) {
    auto& s=shared_;auto& r=*resource_;
    const auto count=c.party.counts()[3];
    if(count>3)throw std::runtime_error("Results party alpha exceeds three source rows");
    const int delta=(quick?128:0)|4;
    auto clipped=r.add_class_rgba(13,0,{keep,keep,keep,delta});
    s.progress.hide_changed(r,0);
    for(unsigned row=1;row<count&&clipped;++row) {
        clipped=r.add_class_rgba(13,int(row),{keep,keep,keep,delta});
        s.progress.hide_changed(r,row);
    }
    if(count&&clipped){s.animation.advance=1;s.animation.timer=0;}
}
void ResultsInterface::notice_motion() {
    auto& r=*resource_;
    const auto y=[&](unsigned group,int id) {
        const auto& w=r.widget(group,id);
        if(!w.runtime)throw std::runtime_error("Results notice position has no runtime owner");
        return signed_word(w.runtime->words[7]);
    };
    if(y(1,0)>300)r.add_class_position(1,-1,keep,-4,keep,keep);
    r.add_class_rgba(1,-1,{keep,keep,keep,16});
    r.add_class_position(13,10,4,keep,keep,keep);
    r.add_class_rgba(13,10,{keep,keep,keep,-16});
    r.add_class_position(13,10,4,keep,700,keep);
    if(y(13,3)>80)for(int cls:{11,0,1,2})r.add_class_position(13,cls,keep,-4,keep,keep);
    r.add_rgba(1,6,{keep,keep,keep,-128});r.add_rgba(1,7,{keep,keep,keep,-128});
}
void ResultsInterface::first_update(std::uint32_t pressed,ResultsUiContext c) {
    auto& s=shared_;auto& a=s.animation;auto& r=*resource_;auto& w=mutable_window();
    words_[0xe8c/4]=0;
    if(!(words_[base+0x50/4]&0x40))return;
    words_[0xe48/4]=0;words_[base+0xd4/4]=1; // Typed parent link.
    auto& state=words_[0xe4c/4];auto& command=words_[base+0x68/4];
    auto& previous=words_[base+0x64/4];
    if(a.advance==1){++state;a.advance=0;}
    const bool confirm=(pressed&0x40)!=0; //1002f8:1f85dc, raw pressed latch.
    switch(signed_word(state)) {
    case 0: {
        w.reset_for_open();w.set_opacity_tween(8.f,1.f,0,128);
        accumulators_[0]=0;const std::array<std::size_t,2> groups{1,13};w.bind_ruw(resource_,groups);
        r.reset_class(1,-1);r.add_class_position(1,-1,keep,300,keep,keep);
        r.add_class_rgba(1,-1,{keep,keep,keep,-128});
        r.set_position(1,5,39,keep);r.set_rgba(1,5,{keep,keep,keep,128});
        for(int cls:{-1,0,1,2,10,11})r.add_class_rgba(13,cls,{keep,keep,keep,-128});
        for(int id:{0,1,2,0x38})r.add_extent(13,id,-600,keep,0,keep);
        s.progress.notice=0;++state;command=previous=0;break;
    }
    case 1:command=1;if(confirm){s.skip=1;a.timer=99999999;}break;
    case 2:command=2;if(confirm||s.skip==1){s.skip=1;a.timer=99999999;}break;
    case 3:
        command=3;
        if(confirm||s.skip==1){s.money_display=s.reward_display=half(c.reward,2);a.timer=99999999;currency(true,c);}
        break;
    case 4:command=4;currency(true,c);if(confirm)a.timer=120;break;
    case 5:
        currency(true,c);command=5;
        if(confirm||s.skip==1){a.timer=99999999;a.party_heading(r,data_,true);}break;
    case 6:
        command=6;if(confirm||s.skip==1){a.timer=99999999;party_alpha(true,c);}break;
    case 7:case 8:case 9:
        command=state;if(s.progress.notice==1){command=0xffffffffu;state=0xfffffffcu;}break;
    case 10:
        command=10;
        if(confirm&&s.loader_ready==1) {
            commit_battle_result_rewards(c.variables,c.reward);
            state=0xffffffffu;w.set_opacity_tween(-4.f,128.f,0,128);
            ++command;previous=command;refresh_game_characters(c.variables,c.tables,0);
        }
        break;
    case -4:
        ++s.progress.notice_wait;
        if(signed_word(s.progress.notice_wait)>360&&confirm) {
            command=previous=7;s.progress.notice=2;state=7;
        }
        break;
    case -1:if(w.opacity()==0.f)state=0xfffffffeu;break;
    case -2:words_[0x1c/4]|=0x80000000u;break;
    default:break;
    }
    ++words_[0xe8c/4];
}
void ResultsInterface::second_update(ResultsUiContext c) {
    if(!(words_[base+0x50/4]&0x40)||!words_[base+0x5c/4])return;
    auto& s=shared_;auto& a=s.animation;auto& r=*resource_;
    s.command=words_[base+0x68/4];std::copy_n(words_.begin()+base+0x8c/4,8,s.arguments.begin());
    ++a.timer;
    switch(signed_word(s.command)) {
    case 1:a.heading(r,data_);break;
    case 2:a.items(r,data_,s.item_count);break;
    case 3:currency(false,c);break;
    case 4:++a.timer;if(signed_word(a.timer)>120){a.advance=1;a.timer=0;}break;
    case 5:a.party_heading(r,data_,false);break;
    case 6:party_alpha(false,c);break;
    case 7:case 8:case 9: {
        if(signed_word(s.progress.notice)>0)notice_motion();
        const unsigned row=s.command-7;
        if((row&&c.party.counts()[3]<=row)||s.progress.update_character(row,false,
            c.variables,c.tables,c.party,r,c.numbers,c.play_jingle)) {a.advance=1;a.timer=0;}
        break;
    }
    case 10:case -1:if(signed_word(s.progress.notice)>0)notice_motion();break;
    default:break;
    }
}
}
