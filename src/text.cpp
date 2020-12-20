#include "okage/text.hpp"
#include <algorithm>
#include <bit>
#include <stdexcept>

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
std::vector<std::uint32_t> MessageBank::message_indices(std::int32_t id) const {
    std::vector<std::size_t> roots;
    for(std::size_t i=1;i<nodes.size();++i)if(nodes[i].depth==0)roots.push_back(i);
    if(roots.empty())throw std::runtime_error("PME has no message roots");
    auto root=roots[std::clamp<std::int64_t>(id,0,roots.size()-1)];
    for(auto candidate:roots) {
        if(__builtin_bit_cast(std::int32_t, (decimal(strings.at(nodes[candidate].string_index),true)))==id) {
            root=candidate;break;
        }
    }
    std::vector<std::uint32_t> result;
    for(std::size_t i=root+1;i<nodes.size() && nodes[i].depth>0;++i) {
        if(nodes[i].depth!=1)continue;
        // Original 0x19b9c8 selects immediate children. 0x19a150 follows only
        // the strictly increasing depth chain from each child, not every branch.
        for(auto j=i;;++j) {
            if(result.size()==256)throw std::runtime_error("PME message exceeds original fragment capacity");
            result.push_back(nodes[j].string_index);
            if(j+1==nodes.size() || nodes[j+1].depth<=nodes[j].depth)break;
        }
    }
    return result;
}
std::vector<std::string> MessageBank::message(std::int32_t id) const {
    std::vector<std::string> result;
    for(auto index:message_indices(id))result.push_back(strings.at(index));
    return result;
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
TextStep DialogueText::step() {
    if(state.flags&0x20)return TextStep::end;
    if(state.flags&0x910 || state.delay)return TextStep::pause;
    while(fragment_<fragments_.size()) {
        const auto& text=fragments_[fragment_];
        if(cursor_==text.size()) {++fragment_;cursor_=0;continue;}
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
