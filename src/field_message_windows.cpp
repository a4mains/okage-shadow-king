#include "okage/field_message_windows.hpp"
#include <algorithm>
#include <bit>
#include <cstring>
#include <stdexcept>

namespace okage {
namespace {
constexpr std::int32_t keep=RuwResource::preserve_component;
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
std::uint32_t word(Bytes data,std::size_t at) {
    if(at>data.size()||data.size()-at<4)throw std::runtime_error("Message window data bounds");
    return unsigned(data[at])|(unsigned(data[at+1])<<8)|(unsigned(data[at+2])<<16)|(unsigned(data[at+3])<<24);
}
bool digest(Bytes data,std::size_t at,std::string_view hex) {
    if(at>data.size()||data.size()-at<hex.size()/2)return false;
    constexpr std::string_view digits="0123456789abcdef";
    for(std::size_t i=0;i<hex.size();++i)
        if(hex[i]!=digits[(data[at+i/2]>>(i%2?0:4))&15])return false;
    return true;
}
//1f15a8 restricted to the reached sysmes formats: exactly one %s.
std::string format_one(std::string_view format,std::string_view argument) {
    const auto at=format.find('%');
    check(at!=format.npos&&format.substr(at,2)=="%s"&&format.find('%',at+2)==format.npos,
          "Message widget format outside the reached single %s conversion");
    std::string out(format.substr(0,at));out+=argument;out+=format.substr(at+2);
    check(out.size()<256,"Message widget text exceeds its 256-byte stack buffer");
    return out;
}
//120d08: -1 for an empty string, else 1 + number of "\n" separators.
int segment_count(std::string_view text) {
    if(text.empty())return -1;
    int count=1;
    for(std::size_t i=0;i<text.size();++i)if(text[i]=='\\'&&i+1<text.size()&&text[i+1]=='n'){++count;++i;}
    return count;
}
//120d68 over a NUL-free string: segment i (empty past the end).
std::string segment(std::string_view text,int index) {
    int current=0;std::size_t start=0;
    for(std::size_t i=0;i<=text.size();++i) {
        const bool end=i==text.size(),separator=!end&&text[i]=='\\'&&i+1<text.size()&&text[i+1]=='n';
        if(end||separator) {
            if(current==index)return std::string(text.substr(start,i-start));
            ++current;if(separator){start=i+2;++i;}
        }
    }
    return {};
}
std::string_view c_string(const std::array<char,256>& bytes) {
    return {bytes.data(),strnlen(bytes.data(),bytes.size())};
}
std::array<std::uint32_t,3> triple(const std::array<std::uint32_t,8>& words,unsigned first) {
    return {words[first],words[first+1],words[first+2]};
}
}

FieldMessageWindowData::FieldMessageWindowData(Bytes data) {
    constexpr std::array<std::uint8_t,8> magic={'O','K','M','S','W','1',0,0};
    constexpr std::size_t header=88,size=header+224+16+12+16+8+14;
    if(data.size()!=size||!std::equal(magic.begin(),magic.end(),data.begin())||
       word(data,8)!=1||word(data,12)!=2||word(data,16)!=28||word(data,20)!=1)
        throw std::runtime_error("Unsupported message window data format");
    if(!digest(data,24,"2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5")||
       !digest(data,56,"9290a28fd31947bbf4827c8adae171544246df6dbdab966f4b2b3110123a3c8e"))
        throw std::runtime_error("Unsupported message window source revision");
    for(unsigned r=0;r<2;++r)for(unsigned i=0;i<28;++i)rows_[r][i]=word(data,header+r*112+i*4);
    for(unsigned r=0;r<2;++r) {
        const auto& row=rows_[r];
        const std::uint32_t capacity_a=r?12:16,capacity_b=r?2:8;
        if(row[0]!=r+1||row[21]!=1||row[22]!=r+1||row[23]!=r+1||row[24]!=r||
           row[25]!=capacity_a||row[26]!=capacity_b||row[20]!=0||row[27]!=0)
            throw std::runtime_error("Unsupported message window host binding or row layout");
    }
    UiWindow::AtlasTuple terminator{};
    for(std::size_t j=0;j<4;++j)terminator[j]=word(data,header+224+j*4);
    if(terminator!=UiWindow::AtlasTuple{})throw std::runtime_error("Unsupported message window atlas entries");
    atlas_.push_back(terminator);
    for(unsigned i=0;i<3;++i)selectors_[i]=word(data,header+240+i*4);
    if(selectors_!=std::array<std::uint32_t,3>{0x27,0x28,0x29})throw std::runtime_error("Unsupported message cursor selectors");
    const auto strings=data.subspan(header+276,14);
    if(!std::equal(strings.begin(),strings.end()," \0NULL\0system"))
        throw std::runtime_error("Unsupported message window literals");
}

FieldMessageWindows::FieldMessageWindows(BattleUiShared& ui,const FieldMessageWindowData& data,
        std::shared_ptr<RuwResource> ruw,std::shared_ptr<const void> texture,Construction construction,
        std::int32_t message_id,FieldMessageGlobals& globals)
    :ui_(ui),ruw_(std::move(ruw)),row_a_(data.row(0)),row_b_(data.row(1)) {
    check(ruw_&&texture,"Field message windows require the field RUW and bm_if owners");
    (void)construction;
    for(unsigned i=0;i<8;++i)layer_a_xyz_[i]=row_a_[4+i];
    //11f570: *0x24f734 = id before 1152a8; 11f7a0 copies it into each layer.
    ui_.atlas_texture=message_id;
    const auto build=[&](const std::array<std::uint32_t,28>& row,const std::shared_ptr<UiWindowStream>& stream,
                         unsigned& slot,const UiWindow*& owner,bool body) {
        const auto free=std::find(ui_.registry.begin(),ui_.registry.end(),nullptr);
        if(free==ui_.registry.end())throw std::runtime_error("Field message UI registry exhausted");
        slot=unsigned(free-ui_.registry.begin());
        auto window=std::make_shared<UiWindow>(row[25],row[26],slot,stream);
        *free=window;owner=window.get();
        if(window->configure(row,data.atlas(),texture))ui_.resource.layout_shared().sprite_dirty=true;
        if(!body) {
            //147ec8 -> 147e80(0x2dbb40): 0x1f9ca0, cursor 0, [14,1f9c00,6,1f9c2c].
            stream->entries={UiStreamCommand{14,UiStreamReference::MenuTransform1f9c00},
                             UiStreamCommand{6,UiStreamReference::MenuTransform1f9c2c}};
            globals.decoration_stage_memo=-1;globals.choice_row=-1;
            globals.speaker_ticks=0;globals.body_settle=0;globals.cursor_counter=0;
        } else {
            stream->entries={UiStreamCommand{14,UiStreamReference::MenuTransform1f9c00},
                             UiStreamCommand{6,UiStreamReference::MenuTransform1f9c1c}};
            globals.choice_row=-1;
            for(int id=0x12;id<=0x14;++id)ruw_->set_rgba(0,id,{0x80,0x80,0x80,0});
        }
        ui_.current_stream=stream->binding_id;ui_.stream_cursor=4;
    };
    try {
        build(row_a_,globals.decoration_stream,slot_a_,window_a_,false);
        build(row_b_,globals.body_stream,slot_b_,window_b_,true);
    } catch(...) {
        for(auto& entry:ui_.registry)if(entry.get()==window_a_||entry.get()==window_b_)entry.reset();
        throw;
    }
}
FieldMessageWindows::~FieldMessageWindows() {
    if(window_a_&&ui_.registry[slot_a_].get()==window_a_)ui_.registry[slot_a_].reset();
    if(window_b_&&ui_.registry[slot_b_].get()==window_b_)ui_.registry[slot_b_].reset();
}
UiWindow& FieldMessageWindows::live(unsigned slot,const UiWindow* expected) const {
    auto& entry=ui_.registry.at(slot);
    if(!expected||entry.get()!=expected)throw std::runtime_error("Field message window lifetime");
    return *entry;
}
const UiWindow& FieldMessageWindows::decoration() const {return live(slot_a_,window_a_);}
const UiWindow& FieldMessageWindows::body() const {return live(slot_b_,window_b_);}
void FieldMessageWindows::bind_decoration(std::size_t group) {
    ui_.resource_accumulators[0]=0;
    const std::array<std::size_t,1> groups{group};
    live(slot_a_,window_a_).bind_ruw(ruw_,groups);
}
void FieldMessageWindows::controllers(unsigned stage,std::int32_t window_type) {
    auto& a=live(slot_a_,window_a_);auto& b=live(slot_b_,window_b_);
    switch(stage) {
    case 0:
        //148958 case0: +ac=-1, 11faf8(-128) -> 1a4d70(-128,128,win,0,0x80),
        //bind RUW group0; falls into case1: the layer angle word9 is reduced
        //then stored 0, +a8=0, 1a5600(win,layer+10,layer+20).
        a.set_opacity_tween(-128.f,128.f,0,0x80);
        bind_decoration(0);
        layer_a_xyz_[5]=0;decoration_counter_=0;
        a.set_transform_xyz(triple(layer_a_xyz_,0),triple(layer_a_xyz_,4));
        b.reset_for_open();b.set_opacity_tween(8.f,1.f,0,0x80);
        a.set_opacity_tween(-128.f,128.f,0,0x80);
        body_text_mode_=true;
        break;
    case 5: {
        a.reset_for_open();
        if(++decoration_counter_>8) {
            decoration_counter_=8;
            if(window_type!=2&&a.opacity()==0.f){a.reset_for_open();a.set_opacity_tween(8.f,1.f,0,0x80);}
        }
        if(a.opacity()==0.f){a.reset_for_open();a.set_opacity_tween(8.f,1.f,0,0x80);}
        break;
    }
    case 6:
        a.set_opacity_tween(-4.f,128.f,0,0x80); //148958 case6: 11faf8(-4.0).
        break;
    case 2:case 4:case 7:case 8:case 9:
        break; //No window effect at these shared stages.
    default:throw std::runtime_error("Field message controller stage outside recovered scope");
    }
}
void FieldMessageWindows::updaters(const FieldMessageBodyState& s,FieldMessageGlobals& g,
                                   const FieldMessageServices& services) {
    check(bool(services.system_message)&&bool(services.number2)&&bool(services.money)&&s.replacement,
          "Field message updater services missing");
    auto& a=live(slot_a_,window_a_);auto& b=live(slot_b_,window_b_);
    auto& r=*ruw_;
    //147fe0: 0x1f9ca0=0x2dbb40, 1a5488(win,1), 1a56d0(win,0x10), rebind on a
    //changed node+ed8.
    ui_.current_stream=1;
    a.set_layout_enabled(true);a.set_transform_parameter(0x10);
    if(g.decoration_stage_memo!=s.window_type) {
        switch(s.window_type) {
        case 0:a.set_layout_enabled(true);bind_decoration(0);break;
        case 1:case 3:a.set_layout_enabled(true);bind_decoration(1);break;
        case 2:case 4:
            a.set_layout_enabled(true);bind_decoration(15);
            for(int id=0;id<3;++id)r.add_rgba(15,id,{keep,keep,keep,-128});
            a.set_transform_xyz(triple(layer_a_xyz_,0),triple(layer_a_xyz_,4));
            break;
        default:break; //-1 before WINTYPE: no binding change.
        }
    }
    g.decoration_stage_memo=s.window_type;

    //146ee8 begins with the speaker (window+1a70) widgets 15..17 of group0.
    b.set_transform_parameter(0x10);
    for(int id=15;id<=17;++id)r.set_text(0,id," ");
    const std::string speaker(s.speaker);
    if(g.last_speaker!=speaker) {
        g.speaker_ticks=0;
        for(int id=15;id<=17;++id)r.set_rgba(0,id,{keep,keep,keep,0});
    }
    g.last_speaker=speaker;
    if(!speaker.empty()&&static_cast<unsigned char>(speaker[0])==0xff) {
        g.speaker_ticks=0;
        for(int id=15;id<=17;++id)r.set_rgba(0,id,{keep,keep,keep,0});
    } else if(++g.speaker_ticks>2) {
        bool listed=false;
        const int count=segment_count(speaker);
        for(int i=0;i<count;++i) {
            const auto part=segment(speaker,i);
            //sysmes 0xcd.. until "NULL": a listed speaker dims widget0.
            int found=0;
            for(unsigned id=0xcd;;) {
                if(part==services.system_message(id)){found=-1;listed=true;break;}
                ++found;++id;
                if(services.system_message(id)=="NULL")break;
                check(id<0x200,"Speaker list has no NULL terminator");
            }
            if(found==-1||listed)r.set_rgba(0,0,{0x14,0x14,0x14,100});
            else r.reset_widget(0,0);
            const auto text=format_one(services.system_message(0x72),part);
            check(15+i<=17,"Speaker exceeds the three recovered widgets");
            if(part!="system")r.set_text(0,15+i,text);
            if(g.speaker_ticks>2)r.add_rgba(0,15+i,{keep,keep,keep,8});
        }
    }
    for(int id=15;id<=17;++id) {
        //1aa4a8 reads the extent written by the preceding layout.
        const auto& w=r.widget(0,id);
        check(w.runtime.has_value(),"Speaker widget runtime missing");
        const auto width=std::bit_cast<std::int32_t>(w.runtime->words[8]);
        r.set_position(0,id,0x4e - width/2,keep);
    }

    //Choice rows 18..20 for a three-segment replacement (window+1b70).
    const auto replacement=c_string(*s.replacement);
    const unsigned segments=replacement_segments(replacement);
    if(segments==3) {
        if(s.choice_accepted==0&&!replacement.empty()) {
            if(g.choice_row==-1)g.choice_row=s.choice_cursor;
            if(s.choice_armed)r.set_rgba(0,3,{0xff,0xff,0xff,0x30});
            else r.set_rgba(0,3,{0xf0,0xf0,100,0});
            r.set_rgba(0,0,{0x28,0xbe,0x2d,keep});
            r.set_position(0,3,keep,s.choice_cursor*0x1b+0x150);
            for(int i=0;i<3;++i) {
                auto line=presentation_name(replacement_segment(*s.replacement,unsigned(i)),s.player_name);
                if(line.empty())line=" ";
                const auto text=format_one(services.system_message(0x106),line);
                //The final unconditional 1aa3c0(1,1) wins over the selection scale.
                r.set_runtime_float_pair(0,0x12+i,1.f,1.f);
                r.set_text(0,0x12+i,text);
                r.set_position(0,0x12+i,0xa4,(i*0x1a+0x160)|(i==2?1:0));
                r.add_rgba(0,0x12+i,{keep,keep,keep,8});
            }
        }
    } else {
        r.set_rgba(0,3,{keep,keep,keep,0});
        if(r.add_rgba(0,0x12,{keep,keep,keep,-32}))g.choice_row=-1;
        r.add_rgba(0,0x13,{keep,keep,keep,-32});
        r.add_rgba(0,0x14,{keep,keep,keep,-32});
    }

    if(s.status==1&&g.body_settle>0)--g.body_settle;
    //Advance cursor widget4.
    r.set_rgba(0,4,{keep,keep,keep,0});
    if(s.status==3)g.body_settle=8;
    if(s.status==2) {
        g.body_settle=8;g.cursor_counter=(g.cursor_counter+1)%0x1e;
        if(segments<2) {
            constexpr std::array<std::uint32_t,3> selectors{0x27,0x28,0x29};
            r.set_runtime_word0(0,4,selectors[unsigned(g.cursor_counter/10)%3]);
            if(s.layer_ready)r.set_rgba(0,4,{keep,keep,keep,0x80});
        }
    }
    //146288: question-choice rows for more than three segments.
    if(!replacement.empty()&&segments>3) {
        check(s.question&&s.question->active,"Question-choice widgets without the native QC owner");
        const auto& q=*s.question;
        r.set_rgba(0,3,{keep,keep,keep,0x30});
        if(s.choice_armed)r.set_rgba(0,3,{0xff,0xff,0xff,0x30});
        else r.set_rgba(0,3,{0xf0,0xf0,100,0});
        r.set_position(0,3,keep,q.highlight_row*0x1b+0x150);
        for(int i=0;i<3;++i) {
            r.set_text(0,0x12+i,format_one(services.system_message(0x72),q.lines[unsigned(i)]));
            const bool big=s.question_flash==0?s.choice_cursor==i:s.question_flash_row==i;
            r.set_runtime_float_pair(0,0x12+i,big?1.1f:1.f,big?1.2f:1.f);
            if(!s.choice_armed)r.set_runtime_float_pair(0,0x12+i,1.f,1.f);
            r.set_rgba(0,0x12+i,{0x80,0x80,0x80,0x80});
        }
    }
    //Description (window+1d70) lines in group15 widgets 0..2.
    const std::string description(s.description);
    if(description.empty()) {
        for(int id=0;id<3;++id)r.add_rgba(15,id,{keep,keep,keep,-8});
    } else {
        const int count=segment_count(description);
        for(int i=0;i<3;++i) {
            if(i>=count){r.add_rgba(15,i,{keep,keep,keep,-8});continue;}
            const auto part=segment(description,i);
            if(g.narration_source[unsigned(i)]!=part)g.narration_ticks[unsigned(i)]=0;
            g.narration_source[unsigned(i)]=part;
            auto line=presentation_name(part,s.player_name);
            if(line.empty())line=" ";
            r.set_runtime_float_pair(15,i,1.5f,1.5f);
            r.set_text(15,i,format_one(services.system_message(0x106),line));
            const auto& w=r.widget(15,i);
            check(w.runtime.has_value(),"Description widget runtime missing");
            const auto width=std::bit_cast<std::int32_t>(w.runtime->words[8]);
            if(width<0)r.set_position(15,i,0,i*0x28+0x50);
            else r.set_position(15,i,300-width/2,i*0x28+0x50);
            auto& ticks=g.narration_ticks[unsigned(i)];
            if(ticks<2)r.set_rgba(15,i,{keep,keep,keep,2});
            else r.add_rgba(15,i,{keep,keep,keep,8});
            if(++ticks>1)ticks=11;
        }
    }
    //HUD widgets 9 (sysmes0x6b with 124200) and 10 (123a90).
    r.set_text(0,9,format_one(services.system_message(0x6b),services.number2(std::min(s.level,99))));
    r.set_text(0,10,services.money(std::max(s.money,0)));
    //146730: body transform by node+ed8 (no draw effect).
    static constexpr std::array<std::array<std::uint32_t,3>,5> body_first{{
        {0xc3320000,0x43750000,0x43828cd9},{0xc38b0000,0xc1900000,0x43828cd9},
        {0xc36c0000,0x42200000,0x43340000},{0xc38b0000,0xc1900000,0x43828cd9},
        {0xc36c0000,0x42200000,0x43340000}}};
    if(s.window_type>=0&&s.window_type<=4)
        b.set_transform_xyz(body_first[unsigned(s.window_type)],{row_b_[8],row_b_[9],row_b_[10]});
}
void FieldMessageWindows::resource_pass(const Pass& pass,const DialogueOpacity& decoration,
        const std::function<void(const TextPlacementHooks*)>& body_frame) {
    auto& a=live(slot_a_,window_a_);auto& b=live(slot_b_,window_b_);
    last_={};
    unsigned draws=0;
    const auto random=[&]{++draws;return pass.random();};
    check(bool(pass.fonts)&&!pass.fonts->empty()&&bool(body_frame),"Field message resource pass owners missing");
    //1980f8 clocks every registry window before any 198390 layout.
    a.advance_clock();
    if(!body_text_mode_)b.advance_clock();
    //The native decoration model and the real window must agree after the clock.
    if(a.opacity()!=decoration.value||std::bit_cast<float>(a.word(0x27c))!=decoration.delta)
        throw std::runtime_error("Field message decoration window opacity differs from the dialogue model");
    const std::span<const Font> fonts(*pass.fonts);
    const auto lay_out_a=[&] {
        const auto before=draws;
        UiLayoutContext context{pass.shared,fonts,pass.sine,random,
            std::bit_cast<std::int32_t>(pass.accumulators[0]),pass.packets};
        last_.a_laid_out=a.prepare_mode2(context);
        if(last_.a_laid_out)++a_layouts_;
        last_.a_draws=draws-before;
    };
    const auto lay_out_b=[&] {
        const auto before=draws;
        if(!body_text_mode_) {
            body_frame(nullptr);
            UiLayoutContext context{pass.shared,fonts,pass.sine,random,0,pass.packets};
            last_.b_constructor=b.prepare_mode2(context);
        } else {
            const UiPlacementContext placement{pass.shared.layer,pass.sine,random};
            TextPlacementHooks hooks;
            hooks.begin=[&]{b.begin_mode1_pass();};
            hooks.glyph=[&](std::uint32_t code,std::int32_t x,std::int32_t y,std::int32_t w,std::int32_t h) {
                b.place_mode1_glyph(std::int32_t(code),x,y,w,h,fonts,placement);++last_.b_glyphs;};
            hooks.page=[&]{b.clear_mode1_page();};
            hooks.tail=[&]{b.finish_mode1_pass(placement);last_.b_pass=true;++b_passes_;};
            body_frame(&hooks);
        }
        last_.b_draws=draws-before;
    };
    if(slot_a_<slot_b_){lay_out_a();lay_out_b();}
    else {lay_out_b();lay_out_a();}
}
}
