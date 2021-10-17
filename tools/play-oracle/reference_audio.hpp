#pragma once
#include "sound/SoundHandler.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
#include <sstream>
#include <stdexcept>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

// Reference PCM sink only. No host audio device and no native game-audio claim.
class RecordedAudio final : public CSoundHandler {
    std::vector<int16> pcm;
    unsigned rate=0;
    bool failed=false;
    struct VblankOffset {unsigned vblank;std::size_t sampleFrames;};
    std::vector<VblankOffset> offsets;
    unsigned indexedVblanks=0;
public:
    void Reset() override {pcm.clear();rate=0;failed=false;offsets.clear();}
    bool HasFreeBuffers() override {return true;}
    void RecycleBuffers() override {}
    void Write(int16* samples,unsigned count,unsigned sampleRate) override {
        if (failed) return;
        if (count%2 || sampleRate!=44100 || (rate && rate!=sampleRate) || pcm.size()+count>44100*2*600) {failed=true;return;}
        rate=sampleRate;
        try {pcm.insert(pcm.end(),samples,samples+count);} catch (...) {failed=true;}
    }
    void prepareVblankIndex(unsigned count) {
        if(!count || count>36000 || !offsets.empty())
            throw std::runtime_error("Invalid reference audio index extent");
        offsets.reserve(count);indexedVblanks=count;
    }
    std::size_t sampleFrameCount() const {return pcm.size()/2;}
    void markVblank(unsigned vblank) {
        if(failed || !indexedVblanks || vblank!=offsets.size()+1 || vblank>indexedVblanks)
            throw std::runtime_error("Invalid reference audio vblank observation");
        offsets.push_back({vblank,sampleFrameCount()});
    }
    // Call after the VM has stopped. Exclusive creation also refuses symlinks.
    void finishVblankIndex(const std::filesystem::path& path) const {
        if(failed || !rate || offsets.size()!=indexedVblanks || !indexedVblanks)
            throw std::runtime_error("Incomplete reference audio vblank index");
        std::ostringstream text;
        text<<"{\n  \"sample_rate\": "<<rate<<", \"channels\": 2, \"total_sample_frames\": "<<sampleFrameCount()
            <<",\n  \"boundary\": \"OnNewFrame after GS SetVBlank and PadHandler Update, before this iteration's EE/IOP update\","
            <<"\n  \"scope\": \"Offsets into emitted interleaved stereo PCM; SPU output is block buffered. Repeated offsets are valid; these are not exact audible event times.\","
            <<"\n  \"vblanks\": [\n";
        for(std::size_t i=0;i<offsets.size();++i) {
            const auto& offset=offsets[i];
            text<<"    {\"vblank_start\": "<<offset.vblank<<", \"sample_frame_offset\": "<<offset.sampleFrames<<"}"
                <<(i+1<offsets.size() ? ",\n" : "\n");
        }
        text<<"  ]\n}\n";
        const auto bytes=text.str();
        const int fd=::open(path.c_str(),O_WRONLY|O_CREAT|O_EXCL,0600);
        if(fd<0)throw std::runtime_error("Cannot create new reference audio index: "+path.string());
        std::size_t written=0;
        while(written<bytes.size()) {
            const auto count=::write(fd,bytes.data()+written,bytes.size()-written);
            if(count<0 && errno==EINTR)continue;
            if(count<=0) {::close(fd);throw std::runtime_error("Could not write reference audio index");}
            written+=std::size_t(count);
        }
        if(::close(fd)!=0)throw std::runtime_error("Could not close reference audio index");
    }
    // Call with the VM paused; Write runs on its emulation thread.
    void finish(const std::filesystem::path& path) const {
        if (failed || !rate || pcm.empty()) throw std::runtime_error("Reference audio capture failed or has no complete blocks");
        if (std::filesystem::exists(path) || std::filesystem::is_symlink(path)) throw std::runtime_error("Refusing existing WAV output");
        std::ofstream out(path,std::ios::binary);
        auto word=[&](unsigned value,unsigned bytes){for (unsigned i=0;i<bytes;++i) out.put(static_cast<char>(value>>(i*8)));};
        const auto bytes=unsigned(pcm.size()*2);
        out.write("RIFF",4);word(bytes+36,4);out.write("WAVEfmt ",8);word(16,4);word(1,2);word(2,2);
        word(rate,4);word(rate*4,4);word(4,2);word(16,2);out.write("data",4);word(bytes,4);
        unsigned peak=0;std::size_t nonzero=0;
        for (auto sample:pcm) {word(static_cast<uint16>(sample),2);peak=std::max(peak,unsigned(std::abs(int(sample))));nonzero+=sample!=0;}
        if (!out) throw std::runtime_error("Could not write reference WAV");
        std::cout<<"audio_frames="<<pcm.size()/2<<" rate="<<rate<<" peak="<<peak<<" nonzero_samples="<<nonzero<<std::endl;
    }
};
