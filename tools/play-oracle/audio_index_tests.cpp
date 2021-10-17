// Standalone sink tests: no VM, disc, or audio device is constructed.
#include "reference_audio.hpp"
#include <iterator>

void require(bool value,const char* why) {if(!value)throw std::runtime_error(why);}
template<class F> void rejects(F function) {
    bool threw=false;try {function();}catch(const std::runtime_error&){threw=true;}
    require(threw,"Expected invalid audio index operation to fail");
}
int main(int argc,char** argv) {try {
    require(argc==2,"Usage: audio-index-tests NEW_DIRECTORY");
    const std::filesystem::path directory=argv[1];
    require(std::filesystem::create_directory(directory),"Test output directory must be fresh");
    RecordedAudio audio;
    rejects([&](){audio.prepareVblankIndex(0);});
    rejects([&](){audio.prepareVblankIndex(36001);});
    audio.prepareVblankIndex(3);
    audio.markVblank(1); // No block has been emitted yet.
    rejects([&](){audio.markVblank(1);});
    rejects([&](){audio.markVblank(3);});
    rejects([&](){audio.finishVblankIndex(directory/"incomplete.json");});
    int16 samples[]={11,-11,22,-22};
    audio.Write(samples,4,44100);
    audio.markVblank(2);
    audio.markVblank(3);
    require(audio.sampleFrameCount()==2,"Index must count stereo frames, not individual samples");
    rejects([&](){audio.markVblank(4);});
    const auto output=directory/"offsets.json";
    audio.finishVblankIndex(output);
    std::ifstream input(output);
    const std::string contents((std::istreambuf_iterator<char>(input)),std::istreambuf_iterator<char>());
    for(const auto* expected:{"\"total_sample_frames\": 2", "\"vblank_start\": 1, \"sample_frame_offset\": 0",
                             "\"vblank_start\": 2, \"sample_frame_offset\": 2",
                             "\"vblank_start\": 3, \"sample_frame_offset\": 2"})
        require(contents.find(expected)!=std::string::npos,"Wrong sample offset in JSON");
    rejects([&](){audio.finishVblankIndex(output);});
    const auto dangling=directory/"dangling.json";
    std::filesystem::create_symlink(directory/"missing.json",dangling);
    rejects([&](){audio.finishVblankIndex(dangling);});
    require(std::filesystem::is_symlink(dangling),"Index replaced a symlink");
    audio.Reset();
    require(audio.sampleFrameCount()==0,"Reset did not clear PCM");
    audio.markVblank(1);
    rejects([&](){audio.finishVblankIndex(directory/"reset-incomplete.json");});
    audio.Write(samples,3,44100);
    rejects([&](){audio.markVblank(2);});
    std::cout<<"Audio vblank index tests passed\n";
    return 0;
} catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}}
