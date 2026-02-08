#pragma once
#include <charconv>
#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace okage {
struct HeadlessCommand {
    enum class Kind { Step, Capture, Status, View, Quit, Observe } kind;
    unsigned ticks=0;
    std::uint16_t held=0;
    std::string name;
};
inline HeadlessCommand parse_headless_command(std::string_view line,unsigned remaining) {
    std::istringstream in{std::string(line)};
    std::string verb,a,b,extra;in>>verb;
    if(verb=="quit" && !(in>>extra))return {HeadlessCommand::Kind::Quit,0,0,{}};
    if(verb=="status" && !(in>>extra))return {HeadlessCommand::Kind::Status,0,0,{}};
    // Read-only diagnostic JSON: actors, interaction and mode. No tick or journal.
    if(verb=="observe" && !(in>>extra))return {HeadlessCommand::Kind::Observe,0,0,{}};
    if(verb=="capture" && (in>>a) && !(in>>extra)) {
        auto folded=a;
        for(auto& c:folded)if(c>='A'&&c<='Z')c=char(c-'A'+'a');
        if(a.empty()||a.size()>64||folded=="final")throw std::runtime_error("Invalid or reserved capture name");
        for(const unsigned char c:a)
            if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'))
                throw std::runtime_error("Capture name requires letters, digits, underscore or hyphen");
        return {HeadlessCommand::Kind::Capture,0,0,a};
    }
    if(verb=="view" && (in>>a) && !(in>>extra)) {
        auto folded=a;
        for(auto& c:folded)if(c>='A'&&c<='Z')c=char(c-'A'+'a');
        if(a.empty()||a.size()>64||folded=="final")throw std::runtime_error("Invalid or reserved view name");
        for(const unsigned char c:a)
            if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'))
                throw std::runtime_error("View name requires letters, digits, underscore or hyphen");
        return {HeadlessCommand::Kind::View,0,0,a};
    }
    if(verb=="step" && (in>>a>>b) && !(in>>extra)) {
        unsigned ticks=0,mask=0;
        const auto [ta,te]=std::from_chars(a.data(),a.data()+a.size(),ticks);
        const auto [ma,me]=std::from_chars(b.data(),b.data()+b.size(),mask,16);
        if(te!=std::errc{}||ta!=a.data()+a.size()||!ticks||ticks>6000||ticks>remaining||
           me!=std::errc{}||ma!=b.data()+b.size()||mask>0xffff)
            throw std::runtime_error("Step requires 1..6000 remaining ticks and a 16-bit hexadecimal pad mask");
        return {HeadlessCommand::Kind::Step,ticks,std::uint16_t(mask),{}};
    }
    throw std::runtime_error("Expected: step TICKS HEXMASK, capture NAME, status, observe, view NAME, or quit");
}
}
