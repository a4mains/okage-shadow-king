#include "okage/text.hpp"
#include <algorithm>
#include <bit>
#include <cfenv>
#include <cmath>
#include <stdexcept>

#pragma STDC FENV_ACCESS ON
#pragma STDC FP_CONTRACT OFF

namespace okage {
namespace {
std::uint32_t word(Bytes b,std::size_t at) {
    if(at>b.size() || b.size()-at<4)throw std::runtime_error("Truncated PME word");
    return b[at] | (std::uint32_t(b[at+1])<<8) | (std::uint32_t(b[at+2])<<16) | (std::uint32_t(b[at+3])<<24);
}
bool space(unsigned char c) {return c==' ' || (c>=9 && c<=13);}
std::uint32_t decimal(std::string_view s,bool sign) {
    std::size_t at=0;while(at<s.size() && space(s[at]))++at;
    bool negative=false;
    if(sign && at<s.size() && (s[at]=='-' || s[at]=='+'))negative=s[at++]=='-';
    std::uint32_t n=0;
    while(at<s.size() && s[at]>='0' && s[at]<='9')n=n*10+unsigned(s[at++]-'0');
    return negative ? 0u-n : n;
}
}
MessageBank read_pme(Bytes b) {
    constexpr std::string_view header="KOME_RXUI_DATA  ";
    if(b.size()<24 || !std::equal(header.begin(),header.end(),b.begin()))
        throw std::runtime_error("Unsupported PME signature");
    const auto count=word(b,16);
    if(count>(b.size()-24)/8)throw std::runtime_error("PME string table bounds");
    const std::size_t tree_at=20+std::size_t(count)*8;
    const auto node_count=word(b,tree_at);
    if(node_count>(b.size()-tree_at-4)/8)throw std::runtime_error("PME node table bounds");
    MessageBank result;
    std::size_t at=tree_at+4+std::size_t(node_count)*8;
    for(std::uint32_t i=0;i<count;++i) {
        const auto index=word(b,20+std::size_t(i)*8),size=word(b,24+std::size_t(i)*8);
        if(index!=i || size==0 || size>b.size()-at || b[at+size-1]!=0)
            throw std::runtime_error("Invalid PME string record");
        if(std::find(b.begin()+at,b.begin()+at+size-1,0)!=b.begin()+at+size-1)
            throw std::runtime_error("Embedded PME string terminator");
        result.strings.emplace_back(reinterpret_cast<const char*>(b.data()+at),size-1);
        at+=size;
    }
    if(at!=b.size())throw std::runtime_error("PME trailing bytes");
    for(std::uint32_t i=0;i<node_count;++i) {
        const std::size_t pos=tree_at+4+std::size_t(i)*8;
        const auto depth=word(b,pos),index=word(b,pos+4);
        if(index>=count || depth>node_count)throw std::runtime_error("Invalid PME node");
        result.nodes.push_back({depth,index});
    }
    return result;
}
MessagePath MessageBank::message_path(std::int32_t id) const {
    std::vector<std::size_t> roots;
    for(std::size_t i=1;i<nodes.size();++i)if(nodes[i].depth==0)roots.push_back(i);
    if(roots.empty())throw std::runtime_error("PME has no message roots");
    auto root=roots[std::clamp<std::int64_t>(id,0,roots.size()-1)];
    for(auto candidate:roots) {
        if(__builtin_bit_cast(std::int32_t, (decimal(strings.at(nodes[candidate].string_index),true)))==id) {
            root=candidate;break;
        }
    }
    MessagePath result;result.root=root;
    for(std::size_t i=root+1;i<nodes.size() && nodes[i].depth>0;++i) {
        if(nodes[i].depth!=1)continue;
        // Original 0x19b9c8 selects immediate children. 0x19a150 follows only
        // the strictly increasing depth chain from each child, not every branch.
        for(auto j=i;;++j) {
            if(result.strings.size()==256)throw std::runtime_error("PME message exceeds original fragment capacity");
            result.nodes[result.strings.size()]=j;result.strings.push_back(nodes[j].string_index);
            if(j+1==nodes.size() || nodes[j+1].depth<=nodes[j].depth)break;
        }
    }
    result.nodes[result.strings.size()]=0xffffffffu;return result;
}
std::vector<std::uint32_t> MessageBank::message_indices(std::int32_t id) const {
    return message_path(id).strings;
}
std::vector<std::string> MessageBank::message(std::int32_t id) const {
    std::vector<std::string> result;
    for(auto index:message_indices(id))result.push_back(strings.at(index));
    return result;
}
unsigned MessageBank::branch_count(const MessagePath& path,std::size_t fragment) const {
    if(fragment>=path.strings.size())throw std::runtime_error("Choice fragment outside message");
    const auto parent=path.nodes[fragment];
    if(parent>=nodes.size())throw std::runtime_error("Choice node outside PME");
    unsigned count=0;const auto depth=nodes[parent].depth;
    for(std::size_t i=parent+1;i<nodes.size()&&nodes[i].depth>depth;++i)
        if(nodes[i].depth==depth+1)++count;
    if(count>512)throw std::runtime_error("Choice child table capacity");
    return count;
}
bool MessageBank::select_branch(MessagePath& path,std::size_t fragment,unsigned selection) const {
    if(fragment>=path.strings.size())throw std::runtime_error("Choice fragment outside message");
    auto children=[&](std::uint32_t parent){
        if(parent>=nodes.size())throw std::runtime_error("Choice node outside PME");
        std::vector<std::uint32_t> result;const auto depth=nodes[parent].depth;
        for(std::size_t i=parent+1;i<nodes.size() && nodes[i].depth>depth;++i)
            if(nodes[i].depth==depth+1)result.push_back(i);
        if(result.size()>512)throw std::runtime_error("Choice child table capacity");
        return result;
    };
    const auto options=children(path.nodes[fragment]);
    if(options.size()<2)return false;
    if(selection>=options.size())throw std::runtime_error("Choice selection outside branches");
    const auto selected=options[selection];const auto roots=children(path.root);
    if(roots.empty())throw std::runtime_error("Choice message has no root children");
    std::size_t rootIndex=0;
    while(rootIndex+1<roots.size() && roots[rootIndex+1]<selected)++rootIndex;
    auto next=path;next.strings.clear();
    for(std::size_t i=rootIndex;i<roots.size();++i){
        for(std::size_t node=i==rootIndex?selected:roots[i];node<nodes.size();++node){
            const bool leaf=node+1==nodes.size() || nodes[node+1].depth<=nodes[node].depth;
            if(next.strings.size()==256)throw std::runtime_error("Choice fragment capacity");
            if(!leaf)next.nodes[next.strings.size()]=node;
            next.strings.push_back(nodes[node].string_index);
            if(leaf)break;
        }
    }
    next.nodes[next.strings.size()]=0xffffffffu;path=std::move(next);return true;
}
DialogueText::DialogueText(const MessageBank& bank,std::int32_t id):bank_(&bank),path_(bank.message_path(id)) {
    for(auto index:path_.strings)fragments_.push_back(bank.strings.at(index));
}
void DialogueText::move_choice(int delta) {
    if(!delta)return;
    int count=1;std::size_t at=0;
    while((at=state.replacement.find("\\n",at))!=std::string::npos){++count;at+=2;}
    const auto next=std::int64_t(choice_cursor)+delta;
    choice_cursor=next<0 ? count-1 : next%count;
}
void DialogueText::set_option_cursor(int selection) {
    if(!bank_||!fragment_)throw std::runtime_error("Option cursor requires a preceding PME fragment");
    // Unlike select_choice's current-node lookup, 0x19b800 refreshes children
    // of path[index-1], then forces zero only when that node has <2 children.
    choice_cursor=bank_->branch_count(path_,fragment_-1)<2 ? 0 : selection;
}
bool DialogueText::select_choice(unsigned selection) {
    if(!bank_)throw std::runtime_error("Choice requires PME node context");
    const bool changed=bank_->select_branch(path_,fragment_,selection);
    choice_result=changed?int(selection):choice_cursor;choice_cursor=0;
    if(!changed)return false;
    fragments_.clear();for(auto index:path_.strings)fragments_.push_back(bank_->strings.at(index));
    fragment_=cursor_=0;loaded_=false;page.clear();glyphs.clear();
    state.flags=(state.flags&0xfffff6c5u)|1u;
    state.color=0x80808080;state.phase=state.x=state.y=state.line_origin=0;
    state.option.assign(1,char(255));return true;
}
bool MessageBank::bind(std::string_view tag,std::string_view value) {
    if(tag.size()>250 || value.size()>4096 || tag.find('\0')!=tag.npos || value.find('\0')!=value.npos)
        throw std::runtime_error("Invalid PME binding string");
    if(nodes.empty())return false;
    const auto prefix=std::string("\\r[")+std::string(tag)+"]";
    for(std::size_t i=1;i<nodes.size() && nodes[i].depth>nodes[0].depth;++i) {
        if(nodes[i].depth!=nodes[0].depth+1)continue;
        auto& text=strings.at(nodes[i].string_index);
        if(text.starts_with(prefix)) {text=prefix+std::string(value);return true;}
    }
    return false;
}
unsigned consume_text_control(std::string_view text,std::size_t& cursor,TextState& state) {
    if(cursor>=text.size())throw std::runtime_error("Text cursor outside fragment");
    const auto c=static_cast<unsigned char>(text[cursor]);
    if(c!='\\') {
        if(c>=32)return 0;
        ++cursor;return 1;
    }
    if(text.size()-cursor<2)throw std::runtime_error("Truncated text escape");
    auto next=state;
    std::size_t end=cursor+2;
    auto argument=[&]() {
        if(end>=text.size() || text[end]!='[')throw std::runtime_error("Missing text argument");
        const auto start=++end;
        while(end<text.size() && text[end]!=']') {
            // The original treats >0x80 as a two-byte character inside strings.
            if(static_cast<unsigned char>(text[end++])>0x80) {
                if(end>=text.size())throw std::runtime_error("Truncated text argument character");
                ++end;
            }
        }
        if(end==text.size())throw std::runtime_error("Unterminated text argument");
        auto value=text.substr(start,end-start);++end;
        if(value.size()>255)throw std::runtime_error("Text argument exceeds original metadata buffer");
        return value;
    };
    unsigned result=1;
    switch(text[cursor+1]) {
        case 'e':++next.progress;end=cursor+1;break; // Original consumes only the slash here.
        case 'n':next.x=next.line_origin;next.y+=next.line_height;break;
        case 'v':next.flags|=0x910;result=2;break;
        case 'b':next.flags|=0x22;result=2;break;
        case 'c': {
            const auto value=decimal(argument(),false);
            if(value==0)next.color=next.previous_color;
            else {next.previous_color=next.color;next.color=value+0x80;}
            break;
        }
        case 'f':next.font=decimal(argument(),false);break;
        case 'x':next.period=decimal(argument(),false);break;
        case 'w':next.delay=decimal(argument(),false);next.phase=next.period;
                 next.period=0xfffffffe;next.flags|=0x4000;result=2;break;
        case 'm':next.speaker=argument();break;
        case 'y':next.window=argument();break;
        case 'o':next.option=argument();break;
        case 'z':next.replacement=argument();break;
        case 'd':next.description=argument();break;
        case 'r':argument();break;
        case 'l':next.x=next.y=next.phase=0;next.window.clear();next.option.clear();
                 next.replacement.clear();break;
        case 't':break;
        default:throw std::runtime_error("Unrecovered text command");
    }
    state=std::move(next);cursor=end;return result;
}
void tick_text_clock(TextState& state,TextClock& clock) {
    if(!(state.flags&0x8000) || clock.display_modes==0)return;
    if(!std::isfinite(clock.scroll) || !std::isfinite(clock.scroll_step) || !std::isfinite(clock.scroll_base_step))
        throw std::runtime_error("Non-finite text scrolling state");
    const auto original_rounding=std::fegetround();
    if(std::fesetround(FE_TOWARDZERO))throw std::runtime_error("Unable to set text rounding");
    if(!(state.flags&0x910)) {
        const float target=static_cast<float>(clock.scroll_target);
        if(clock.scroll<target && clock.scroll_step!=0 && clock.scroll_base_step!=0) {
            clock.scroll+=clock.scroll_step;
            if(clock.scroll>target)clock.scroll=target;
            if(!(state.flags&0x20))state.flags|=1;
        } else if(state.delay) {
            --state.delay;
        } else {
            ++state.phase;
            if(__builtin_bit_cast(std::int32_t, (state.phase))>__builtin_bit_cast(std::int32_t, (state.period))) {
                state.phase=0;
                clock.revealed+=__builtin_bit_cast(std::int32_t, (state.period))<0 ? 0u-state.period : 1;
            }
            if(!(state.flags&0x20))state.flags|=1;
        }
    }
    state.flags&=~8u;
    std::fesetround(original_rounding);
}
void DialogueText::seek(TextPosition p) {
    if(p.fragment>fragments_.size() || (p.fragment==fragments_.size() && (p.cursor || p.loaded)) ||
       (p.fragment<fragments_.size() && p.cursor>fragments_[p.fragment].size()) || (!p.loaded && p.cursor))
        throw std::runtime_error("Invalid text position");
    fragment_=p.fragment;cursor_=p.cursor;loaded_=p.loaded;page.clear();glyphs.clear();
}
void DialogueText::frame(TextClock& clock,bool visible) {
    tick_text_clock(state,clock);
    if(!(state.flags&0x8000) || !((clock.display_modes>>4)&3) || (state.flags&0x20) || !(state.flags&1))return;
    if(visible) {
        const auto phase=state.phase,period=state.period;
        auto remaining=__builtin_bit_cast(std::int32_t, (period));
        if(remaining<0)++remaining;
        if(fragment_==fragments_.size()) {state.flags=(state.flags|0x22)&~1u;return;}
        if(__builtin_bit_cast(std::int32_t, (phase))<1 && !state.delay) {
            for(unsigned budget=0;;++budget) {
                if(budget==100000)throw std::runtime_error("Text frame instruction limit");
                if(fragment_==fragments_.size())break;
                if(!loaded_) {loaded_=true;cursor_=0;break;}
                const auto& text=fragments_[fragment_];
                if(cursor_==text.size()) {++fragment_;cursor_=0;loaded_=false;continue;}
                const auto old=cursor_;
                const auto control=consume_text_control(text,cursor_,state);
                if(control) {
                    if(text[old]=='\\') {
                        if(text[old+1]=='l') {page.clear();glyphs.clear();clock.revealed=0;clock.scroll=0;clock.scroll_target=0;}
                        if(text[old+1]=='n')page+='\n';
                    }
                    if(cursor_==text.size()) {++fragment_;cursor_=0;loaded_=false;}
                    if(control==2)break;
                    continue;
                }
                unsigned glyph=static_cast<unsigned char>(text[cursor_]);
                const unsigned width=glyph>=0x80 ? 2 : 1;
                if(width>text.size()-cursor_)throw std::runtime_error("Truncated text glyph");
                if(width==2)glyph=(glyph<<8)|static_cast<unsigned char>(text[cursor_+1]);
                if(!fonts.empty()) {
                    if(width!=1 || (clock.glyph_flags&0x84) || clock.scroll!=0)
                        throw std::runtime_error("Unrecovered text layout mode");
                    if(state.font>=fonts.size())throw std::runtime_error("Unbound text font");
                    const auto* found=fonts[state.font].find(glyph);
                    if(!found)throw std::runtime_error("Missing text glyph");
                    const auto& m=found->metrics;
                    auto x=__builtin_bit_cast(std::int32_t, (state.x)),y=__builtin_bit_cast(std::int32_t, (state.y));
                    const int glyph_width=glyph==32 ? 10 : m[8]/16;
                    const int glyph_height=glyph==32 ? state.line_height : (m[9]>>2)/2;
                    if((state.flags&0x40) && std::int64_t(x)+glyph_width>32*24) {
                        x=0;y=__builtin_bit_cast(std::int32_t, (state.y+state.line_height));
                    }
                    // 0x19c568: normal unit-scale placement, including baseline
                    // and left bearing. Space has no bearing or baseline offset.
                    const int bearing=glyph==32 ? 0 : __builtin_bit_cast(std::int16_t, (m[2]))/16;
                    const int baseline=glyph==32 ? 0 : __builtin_bit_cast(std::int16_t, (m[1]))/8;
                    const auto px=std::int64_t(x)-bearing+24,py=std::int64_t(y)-baseline+36;
                    if(px<INT32_MIN || px>INT32_MAX || py<INT32_MIN || py>INT32_MAX)
                        throw std::runtime_error("Text layout coordinate overflow");
                    glyphs.push_back({glyph,state.font,state.color,std::int32_t(px),std::int32_t(py),glyph_width,glyph_height});
                    state.x=__builtin_bit_cast(std::uint32_t, (x))+1u+glyph_width;state.y=__builtin_bit_cast(std::uint32_t, (y));
                }
                page.append(text,cursor_,width);cursor_+=width;
                if(cursor_==text.size()) {++fragment_;cursor_=0;loaded_=false;}
                if(remaining<0) {++remaining;continue;}
                if((glyph==0x20 || glyph==0x8140) && !(clock.glyph_flags&0x20))continue;
                break;
            }
        }
        state.phase=phase;state.period=period;state.flags|=0x80;
    }
    state.flags&=~1u;
}
void DialogueText::confirm(std::int32_t normal_period) {
    if(__builtin_bit_cast(std::int32_t, (state.delay))>0)return;
    if(state.delay || (state.flags&0x910)) {resume();state.period=__builtin_bit_cast(std::uint32_t, (normal_period));}
    else state.period=0xfffffffcu;
}
Texture rasterize_text(std::span<const Font> fonts,std::span<const TextGlyph> glyphs,unsigned width,unsigned height) {
    if(!width || !height || width>4096 || height>4096)throw std::runtime_error("Text surface bounds");
    Texture result{width,height,0,0,std::vector<std::uint8_t>(std::size_t(width)*height*4)};
    for(const auto& g:glyphs) {
        if(g.code==32 || !g.width || !g.height)continue;
        if(g.font>=fonts.size() || g.width<0 || g.height<0)throw std::runtime_error("Text glyph render bounds");
        const auto& f=fonts[g.font];const auto* metric=f.find(g.code);
        if(!metric)throw std::runtime_error("Text glyph unavailable");
        const auto& m=metric->metrics;
        const int sx=m[4]/16,sy=m[5]/16,sw=(m[6]-m[4])/16,sh=(m[7]-m[5])/16;
        if(sw<0 || sh<0 || std::uint64_t(sx)+sw>f.atlas.width || std::uint64_t(sy)+sh>f.atlas.height ||
           f.atlas.rgba.size()!=std::uint64_t(f.atlas.width)*f.atlas.height*4)
            throw std::runtime_error("Text glyph atlas bounds");
        if(!sw || !sh)continue;
        const int left=std::max<std::int64_t>(0,g.x),top=std::max<std::int64_t>(0,g.y);
        const int right=std::min<std::int64_t>(width,std::int64_t(g.x)+g.width),bottom=std::min<std::int64_t>(height,std::int64_t(g.y)+g.height);
        for(int y=top;y<bottom;++y)for(int x=left;x<right;++x) {
            const auto tx=sx+(std::int64_t(x)-g.x)*sw/g.width,ty=sy+(std::int64_t(y)-g.y)*sh/g.height;
            const unsigned alpha=std::min(255u,unsigned(f.atlas.rgba[(ty*f.atlas.width+tx)*4+3])*((g.color>>24)&0xfc)/128);
            auto* dst=result.rgba.data()+(std::size_t(y)*width+x)*4;
            const unsigned old=dst[3]*(255-alpha),total=alpha*255+old;
            if(!total)continue;
            for(unsigned c=0;c<3;++c) {
                const unsigned color=std::min(255u,((g.color>>(8*c))&0xfc)*2);
                dst[c]=(color*alpha*255+dst[c]*old+total/2)/total;
            }
            dst[3]=(total+127)/255;
        }
    }
    return result;
}
TextStep DialogueText::step() {
    if(state.flags&0x20)return TextStep::end;
    if(state.flags&0x910 || state.delay)return TextStep::pause;
    while(fragment_<fragments_.size()) {
        const auto& text=fragments_[fragment_];
        if(cursor_==text.size()) {++fragment_;cursor_=0;loaded_=false;continue;}
        loaded_=true;
        const auto old=cursor_;
        const auto control=consume_text_control(text,cursor_,state);
        if(control) {
            if(text[old]=='\\') {
                if(text[old+1]=='l')page.clear();
                if(text[old+1]=='n')page+='\n';
            }
            if(control==2)return (state.flags&0x20) ? TextStep::end : TextStep::pause;
            // The original \e branch advances only one byte. The remaining 'e'
            // is processed as a glyph by 0x19d0e8, usually in a transient color.
            continue;
        }
        const unsigned width=static_cast<unsigned char>(text[cursor_])>=0x80 ? 2 : 1;
        if(width>text.size()-cursor_)throw std::runtime_error("Truncated text glyph");
        page.append(text,cursor_,width);cursor_+=width;return TextStep::glyph;
    }
    state.flags|=0x22;return TextStep::end;
}
void DialogueText::resume() {
    state.period=2;state.delay=0;state.flags&=0xfffff6ef;state.phase=0;
}
}
