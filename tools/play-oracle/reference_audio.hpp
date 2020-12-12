#pragma once
#include "sound/SoundHandler.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

// Reference PCM sink only. No host audio device and no native game-audio claim.
class RecordedAudio final : public CSoundHandler {
    std::vector<int16> pcm;
    unsigned rate=0;
    bool failed=false;
public:
    void Reset() override {pcm.clear();rate=0;failed=false;}
    bool HasFreeBuffers() override {return true;}
    void RecycleBuffers() override {}
    void Write(int16* samples,unsigned count,unsigned sampleRate) override {
        if (failed) return;
        if (count%2 || sampleRate!=44100 || (rate && rate!=sampleRate) || pcm.size()+count>44100*2*600) {failed=true;return;}
        rate=sampleRate;
        try {pcm.insert(pcm.end(),samples,samples+count);} catch (...) {failed=true;}
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
