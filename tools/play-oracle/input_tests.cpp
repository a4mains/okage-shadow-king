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
    std::cout<<"Recorded input timing, release, neutral axes and malformed rows verified\n";
} catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}}
