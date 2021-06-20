#include "reference_input.hpp"
#include <stdexcept>

struct Pad final : CPadInterface {
    std::array<uint8,PS2::CControllerInfo::MAX_BUTTONS> state{};
    void SetButtonState(unsigned p,PS2::CControllerInfo::BUTTON b,bool v,uint8*) override {if(p)throw std::runtime_error("Wrong port");state.at(b)=v;}
    void SetAxisState(unsigned p,PS2::CControllerInfo::BUTTON b,uint8 v,uint8*) override {if(p)throw std::runtime_error("Wrong port");state.at(b)=v;}
    void GetVibration(unsigned,uint8&,uint8&) override {}
};
int main() {try {
    const auto directory=std::filesystem::current_path()/"input-test-fixtures";
    std::filesystem::create_directories(directory);
    const auto path=directory/"events.input";
    auto write=[&](const char* text){std::ofstream out(path);out<<text;if(!out)throw std::runtime_error("Fixture write failed");};
    write("# fixture\n3 cross 1\n3 left_x 12\n5 cross 0\n");
    RecordedPad recording(path,5);Pad listener;recording.InsertListener(&listener);
    for (unsigned frame=1;frame<=5;++frame) {
        recording.Update(nullptr);
        if (listener.state[PS2::CControllerInfo::CROSS]!=(frame>=3 && frame<5) ||
            listener.state[PS2::CControllerInfo::ANALOG_LEFT_X]!=(frame>=3 ? 12 : 128) ||
            listener.state[PS2::CControllerInfo::ANALOG_RIGHT_Y]!=128)
            throw std::runtime_error("Recorded input transition mismatch");
    }
    for (const auto* invalid:{"0 cross 1", "6 cross 1", "3 cross 2", "3 left_x 256", "3 unknown 1",
        "3 cross 1 extra", "3 cross 1\n2 cross 0", "3 cross 1\n3 cross 0", "-1 cross 1"}) {
        write(invalid);bool threw=false;try {RecordedPad bad(path,5);}catch(const std::runtime_error&){threw=true;}
        if (!threw) throw std::runtime_error("Accepted invalid recording");
    }
    const std::array<bool,4> right={false,false,false,true},up={true,false,false,false},neutral={};
    if(ReferenceWalk::steer(-10,0,.5f)!=right||ReferenceWalk::steer(-10,0,.75f)!=up||
       ReferenceWalk::steer(0,10,0)!=up||ReferenceWalk::steer(NAN,0,0)!=neutral)
        throw std::runtime_error("Camera-relative waypoint steering mismatch");
    const auto route=directory/"walk.route",record=directory/"walk.input";
    {std::ofstream out(route);out<<"1\n-10 0 1\n";}
    std::filesystem::remove(record);
    std::vector<std::uint8_t> ram(0x2000000);
    auto put=[&](unsigned at,std::uint32_t value){std::memcpy(ram.data()+at,&value,4);};
    auto scalar=[&](unsigned at,float value){std::memcpy(ram.data()+at,&value,4);};
    put(0x2d9bf4,0x100000);put(0x100014,0x110000);put(0x100044,0x120000);
    put(0x110044,1);put(0x21301c,0x400000);put(0x33760c,0x800000);
    put(0x337614,0x600000);put(0x337d8c,2);
    scalar(0x800000+3*0x90+0x24,.5f);
    {
        ReferenceWalk walking(route,record);
        const auto before=ram;
        const auto buttons=walking.sample(ram.data(),1);walking.record(1,buttons);
        if(buttons!=right||ram!=before)throw std::runtime_error("Waypoint sampling changed guest RAM or direction");
        put(0x600000,0x100);
        const auto held=walking.sample(ram.data(),2);walking.record(2,held);
        if(held!=neutral)throw std::runtime_error("Waypoint driver did not release for event");
        put(0x600000,0);
        if(walking.sample(ram.data(),3)!=neutral)throw std::runtime_error("Stopped waypoint driver resumed");
    }
    RecordedPad walkReplay(record,3);Pad walkListener;walkReplay.InsertListener(&walkListener);
    for(unsigned frame=1;frame<=3;++frame) {
        walkReplay.Update(nullptr);
        if(bool(walkListener.state[PS2::CControllerInfo::DPAD_RIGHT])!=(frame==1))
            throw std::runtime_error("Waypoint recording is not plain-pad replayable");
    }
    auto walker=[&](const char* name) {
        const auto output=directory/name;std::filesystem::remove(output);
        return ReferenceWalk(route,output);
    };
    {
        auto walking=walker("stationary.input");
        for(unsigned frame=1;frame<=120;++frame)if(walking.sample(ram.data(),frame)!=right)
            throw std::runtime_error("Waypoint stagnation stopped too early");
        if(walking.sample(ram.data(),121)!=neutral)throw std::runtime_error("Waypoint stagnation did not stop");
    }
    {
        auto walking=walker("scene.input");put(0x110044,2);
        if(walking.sample(ram.data(),1)!=neutral)throw std::runtime_error("Waypoint driver crossed scene boundary");
        put(0x110044,1);
    }
    {
        auto walking=walker("arrival.input");scalar(0x800000+0x90+0x20,-10);
        if(walking.sample(ram.data(),1)!=neutral)throw std::runtime_error("Waypoint arrival did not release");
        scalar(0x800000+0x90+0x20,0);
    }
    {
        auto walking=walker("bounds.input");put(0x33760c,0xfffffff0);
        if(walking.sample(ram.data(),1)!=neutral)throw std::runtime_error("Invalid waypoint pointers not rejected");
        put(0x33760c,0x800000);
    }
    for(const auto* invalid:{"1\n", "1\n0 0 0\n", "1\n0 0 -1\n", "1\n0 0 21\n", "1\n0 0 1 extra\n", "1 extra\n0 0 1\n"}) {
        {std::ofstream out(route);out<<invalid;}
        std::filesystem::remove(directory/"unexpected-walk.input");
        bool threw=false;try {ReferenceWalk bad(route,directory/"unexpected-walk.input");}catch(const std::runtime_error&){threw=true;}
        if(!threw)throw std::runtime_error("Accepted invalid waypoint route");
    }
    std::cout<<"Recorded input timing, release, neutral axes and malformed rows verified\n";
} catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}}
