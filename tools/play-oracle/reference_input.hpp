#pragma once
#include "PadHandler.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

class RecordedPad final : public CPadHandler {
    struct Event {unsigned frame,control,value;};
    std::vector<Event> events;
    std::array<uint8,PS2::CControllerInfo::MAX_BUTTONS> state{};
    std::size_t next=0;
    unsigned frame=0;
public:
    RecordedPad(const std::filesystem::path& path,unsigned duration) {
        for (unsigned i=0;i<4;++i) state[i]=128;
        if (path.empty()) return;
        if (std::filesystem::file_size(path)>1024*1024) throw std::runtime_error("Input recording exceeds limit");
        std::ifstream input(path);
        if (!input) throw std::runtime_error("Cannot read input recording");
        const std::array<std::string,20> names={"left_x","left_y","right_x","right_y","up","down","left","right",
            "select","start","square","triangle","circle","cross","l1","l2","l3","r1","r2","r3"};
        std::string line;
        while (std::getline(input,line)) {
            line.erase(line.find('#')==std::string::npos ? line.size() : line.find('#'));
            std::istringstream row(line);row>>std::ws;if (row.eof()) continue;
            Event event{};std::string control,extra;
            if (!(row>>event.frame>>control>>event.value) || row>>extra)
                throw std::runtime_error("Input rows require FRAME CONTROL VALUE");
            const auto it=std::find(names.begin(),names.end(),control);
            if (it==names.end()) throw std::runtime_error("Unknown recorded control: "+control);
            event.control=std::distance(names.begin(),it);
            if (!event.frame || event.frame>duration || event.value>(event.control<4 ? 255U : 1U) ||
                (!events.empty() && event.frame<events.back().frame))
                throw std::runtime_error("Input recording is out of range or unsorted");
            for (auto prior=events.rbegin();prior!=events.rend() && prior->frame==event.frame;++prior)
                if (prior->control==event.control) throw std::runtime_error("Duplicate recorded control at same frame");
            events.push_back(event);
        }
    }
    void Update(uint8* ram) override {
        ++frame;bool changed=frame==1;
        while (next<events.size() && events[next].frame==frame) {
            const auto& event=events[next++];state[event.control]=event.value;changed=true;
        }
        for (auto* listener:m_interfaces) for (unsigned i=0;i<state.size();++i) {
            const auto button=static_cast<PS2::CControllerInfo::BUTTON>(i);
            if (PS2::CControllerInfo::IsAxis(button)) listener->SetAxisState(0,button,state[i],ram);
            else listener->SetButtonState(0,button,state[i]!=0,ram);
        }
        if (changed) {
            unsigned mask=0;
            for (unsigned i=4;i<state.size();++i) if (state[i]) mask|=CPadInterface::GetButtonMask(static_cast<PS2::CControllerInfo::BUTTON>(i));
            std::cout<<"pad_vblank="<<frame<<" pressed_mask=0x"<<std::hex<<mask<<std::dec
                     <<" axes="<<unsigned(state[0])<<","<<unsigned(state[1])<<","<<unsigned(state[2])<<","<<unsigned(state[3])<<std::endl;
        }
    }
};
