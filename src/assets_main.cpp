#include "okage/assets.hpp"
#include "okage/model.hpp"
#include "okage/pose.hpp"
#include "okage/animation.hpp"
#include "okage/texture_animation.hpp"
#include "okage/render.hpp"
#include "okage/map.hpp"
#include "okage/event.hpp"
#include "okage/audio.hpp"
#include "okage/sequence.hpp"
#include "okage/bank.hpp"
#include "okage/music.hpp"
#include "okage/stream_bank.hpp"
#include "okage/text.hpp"
#include "okage/font.hpp"
#include "okage/dialogue.hpp"
#include "okage/camera.hpp"
#include "okage/field.hpp"
#include <algorithm>
#include <bit>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace fs = std::filesystem;
std::vector<std::uint8_t> read(const fs::path& path) {
    const auto size = fs::file_size(path);
    if (size > 128 * 1024 * 1024) throw std::runtime_error("Input exceeds 128 MiB limit");
    std::vector<std::uint8_t> bytes(size);
    std::ifstream file(path, std::ios::binary);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), bytes.size()))
        throw std::runtime_error("Cannot read " + path.string());
    return bytes;
}
void write(const fs::path& path, okage::Bytes bytes) {
    if (fs::is_symlink(path)) throw std::runtime_error("Refusing symlink output");
    if (fs::exists(path)) {
        const auto old = read(path);
        if (old.size() != bytes.size() || !std::equal(old.begin(), old.end(), bytes.begin()))
            throw std::runtime_error("Refusing different existing output: " + path.string());
        return;
    }
    if (!path.parent_path().empty()) fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    if (!file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size()))
        throw std::runtime_error("Cannot write " + path.string());
}
void model_report(const fs::path& path) {
    const auto model = okage::read_xmd(read(path));
    std::cout << "file\t" << path.string() << '\t' << model.textures.size() << '\t' << model.models.size() << '\n';
    for (const auto& part : model.models) {
        std::cout << "model\t" << part.offset << '\t' << part.end << '\t' << part.bones.size() << '\t'
                  << part.hierarchy.size() << '\t' << part.names.size() << '\t' << part.blocks.size() << '\n';
        for (const auto& block : part.blocks) {
            std::size_t triangles=0, quads=0;
            for (const auto& polygon : block.polygons) (polygon.corners.size()==3 ? triangles : quads)++;
            std::cout << "block\t" << block.offset << '\t' << block.end << '\t' << block.draw_offset << '\t'
                      << block.texture_command << '\t' << block.material_command;
            for (const auto value : block.attribute_extents) std::cout << '\t' << value;
            std::cout << '\t' << block.commands.size() << '\t' << triangles << '\t' << quads << '\n';
        }
    }
}
int main(int argc, char** argv) {
    try {
        if (argc < 3) throw std::runtime_error("Usage: okage-assets list ARCHIVE | extract ARCHIVE OUTDIR | decode PACKED OUTFILE | texture XIM PAMFILE");
        const std::string command = argv[1];
        if(command=="stream-bank" && argc==4) {
            const auto bank=okage::read_stream_bank(read(argv[2]));
            std::vector<std::uint32_t> out(bank.chunks.begin(),bank.chunks.end());
            out.push_back(bank.body_size);out.push_back(bank.records.size());
            for(unsigned i=0;i<bank.records.size();++i){out.push_back(bank.records[i]);out.push_back(bank.record(i).size());}
            out.push_back(bank.record(-1).size());out.push_back(bank.record(bank.records.size()).size());
            write(argv[3],okage::Bytes(reinterpret_cast<const std::uint8_t*>(out.data()),out.size()*4));return 0;
        }
        if(command=="stream-cue" && argc==8) {
            const auto bank=okage::read_stream_bank(read(argv[2]));const auto index=std::stoi(argv[3]);
            const auto speed=std::stof(argv[4]);const auto gain=std::stoul(argv[5]);const auto frames=std::stoul(argv[6]);
            if(frames>480000 || gain>128)throw std::runtime_error("Stream cue render budget/gain");
            const auto tone=okage::stream_tone(bank,index,speed,gain);okage::StreamPlayer player(bank);player.play(index,speed,gain);
            std::vector<std::int32_t> mix(frames*2);player.mix(mix);std::vector<std::int16_t> pcm;
            for(const auto value:mix)pcm.push_back(std::clamp(value,-32768,32767));
            write(argv[7],okage::Bytes(reinterpret_cast<const std::uint8_t*>(pcm.data()),pcm.size()*2));
            std::cout<<tone.offset<<' '<<tone.size<<' '<<tone.sample_rate<<' '<<tone.pitch<<' '<<tone.left<<' '<<tone.right<<' '<<tone.adsr1<<' '<<tone.adsr2<<' '<<player.active()<<'\n';return 0;
        }
        if(command=="music-fade" && argc==4) {
            const auto input=read(argv[2]);if(input.empty() || input.size()%16)throw std::runtime_error("Music fade fixture bounds");
            std::vector<std::uint32_t> out;
            for(std::size_t at=0;at<input.size();at+=16){
                std::array<std::uint32_t,4> r;std::memcpy(r.data(),input.data()+at,16);
                if(r[0]>0x800000 || r[3]>10000)throw std::runtime_error("Music fade fixture range");
                okage::MusicVolumeRamp ramp;ramp.current=r[0];ramp.request(r[1],r[2]);
                for(unsigned i=0;i<r[3];++i){
                    const bool send=ramp.current!=ramp.target;ramp.tick();const auto gain=send?ramp.level():0xffffffffu;
                    out.insert(out.end(),{unsigned(ramp.current),unsigned(ramp.target),unsigned(ramp.step),unsigned(ramp.stopped),unsigned(send),gain,gain});
                }
            }
            write(argv[3],okage::Bytes(reinterpret_cast<const std::uint8_t*>(out.data()),out.size()*4));return 0;
        }
        if(command=="music-dry" && argc==6) {
            const auto ticks=std::stoul(argv[4]);if(!ticks || ticks>12000)throw std::runtime_error("Dry music tick budget");
            okage::DryMusicPlayer player(read(argv[2]),okage::read_music_bank(read(argv[3])));
            std::vector<std::int16_t> pcm(ticks*800*2);player.render(pcm);
            write(argv[5],okage::Bytes(reinterpret_cast<const std::uint8_t*>(pcm.data()),pcm.size()*2));
            std::cout<<"Native dry music: ticks="<<player.driver_ticks()<<" peak_voices="<<player.peak_voices()<<" stolen="<<player.stolen_voices()<<" sample_loops="<<player.sample_loops()<<"; reverb/SFX/device pending\n";return 0;
        }
        if(command=="voice-table" && argc==3) {
            const auto& t=okage::gaussian_table();write(argv[2],okage::Bytes(reinterpret_cast<const std::uint8_t*>(t.data()),sizeof(t)));return 0;
        }
        if(command=="voice-envelope" && argc==7) {
            okage::VoiceEnvelope e(std::stoul(argv[2],nullptr,0),std::stoul(argv[3],nullptr,0));
            const auto release=std::stoul(argv[4]),count=std::stoul(argv[5]);if(count>480000)throw std::runtime_error("Envelope tick budget");std::vector<std::uint32_t> out;
            for(unsigned i=0;i<count;++i){if(i==release)e.release();e.tick();out.insert(out.end(),{std::uint32_t(e.level()),std::uint32_t(e.phase()),e.counter()});}
            write(argv[6],okage::Bytes(reinterpret_cast<const std::uint8_t*>(out.data()),out.size()*4));return 0;
        }
        if(command=="voice-sample" && argc==10) {
            const auto b=read(argv[2]);okage::SampleVoice v(b,std::stoul(argv[3],nullptr,0),std::stoul(argv[4],nullptr,0));
            v.parameters(std::stoul(argv[5]),std::stoi(argv[6]),std::stoi(argv[6]));
            const auto release=std::stoul(argv[7]),count=std::stoul(argv[8]);if(count>480000)throw std::runtime_error("Voice sample budget");std::vector<std::int16_t> out;
            for(unsigned i=0;i<count;++i){if(i==release)v.release();const auto p=v.tick();out.push_back(std::clamp(p[0],-32768,32767));out.push_back(std::clamp(p[1],-32768,32767));}
            write(argv[9],okage::Bytes(reinterpret_cast<const std::uint8_t*>(out.data()),out.size()*2));return 0;
        }
        if(command=="music-pitch" && argc==4) {
            const auto input=read(argv[2]);if(input.empty() || input.size()%16)throw std::runtime_error("Pitch fixture extent");
            std::vector<std::uint32_t> out;
            for(std::size_t at=0;at<input.size();at+=16){std::array<std::uint32_t,4> p;std::memcpy(p.data(),input.data()+at,16);out.push_back(okage::music_note_pitch(p[0],p[1],p[2],p[3]));}
            write(argv[3],okage::Bytes(reinterpret_cast<const std::uint8_t*>(out.data()),out.size()*4));return 0;
        }
        if(command=="music-tone" && argc==5) {
            const auto bank=okage::read_music_bank(read(argv[2]));const auto input=read(argv[3]);
            if(input.empty() || input.size()%64)throw std::runtime_error("Tone fixture extent");std::vector<std::uint32_t> out;
            for(std::size_t at=0;at<input.size();at+=64){
                std::array<std::uint32_t,16> row;std::memcpy(row.data(),input.data()+at,64);
                okage::MusicChannel c;const auto* b=input.data()+at+16;
                c.volume=b[2];c.pan=b[3];c.expression=b[8];c.pitch=b[10];c.left=b[20]|(b[21]<<8);c.right=b[22]|(b[23]<<8);
                const auto& t=bank.programs.at(row[0]).at(row[1]);const auto p=okage::music_tone_parameters(bank,t,row[2],row[3],c,row[14],row[15]);
                const auto pitch=std::uint16_t((okage::music_note_pitch(p.center,p.fine,p.key,p.key_fine)*std::int32_t(p.pitch_scale))>>12);
                for(auto v:{t.sample_offset,std::uint32_t(t.adsr1),std::uint32_t(t.adsr2),std::uint32_t(std::uint16_t(p.left)),std::uint32_t(std::uint16_t(p.right)),std::uint32_t(pitch)})out.push_back(v);
            }
            write(argv[4],okage::Bytes(reinterpret_cast<const std::uint8_t*>(out.data()),out.size()*4));return 0;
        }
        if(command=="music-bank" && argc==3) {
            const auto bank=okage::read_music_bank(read(argv[2]));
            for(unsigned i=0;i<bank.programs.size();++i)for(const auto& t:bank.programs[i])
                std::cout<<i<<'\t'<<unsigned(t.key_low)<<'\t'<<unsigned(t.key_high)<<'\t'<<t.sample_offset<<'\t'<<t.adsr1<<'\t'<<t.adsr2<<'\t'<<unsigned(t.center)<<'\t'<<unsigned(t.fine)<<'\t'<<t.pitch_scale<<'\n';
            return 0;
        }
        if(command=="music-sequence" && argc==5) {
            const auto ticks=std::stoul(argv[3]);if(!ticks || ticks>12000)throw std::runtime_error("Music tick budget");
            okage::MusicSequence sequence(read(argv[2]));std::vector<std::uint32_t> output;
            for(unsigned tick=1;tick<=ticks && !sequence.ended();++tick)sequence.tick([&](unsigned phase,const auto& s){
                const std::array<std::uint32_t,10> row={tick,phase,s.cursor(),__builtin_bit_cast(std::uint32_t, (s.remaining())),
                    s.tempo(),s.delta_scale(),s.clock_step(),s.status(),phase==1 ? s.next_status() : 0u,std::uint32_t(s.ended())};
                output.insert(output.end(),row.begin(),row.end());
                for(const auto& channel:s.channels){const auto words=channel.words();output.insert(output.end(),words.begin(),words.end());}
            });
            write(argv[4],okage::Bytes(reinterpret_cast<const std::uint8_t*>(output.data()),output.size()*4));return 0;
        }
        if(command=="field-projection" && argc==4) {
            const auto p=okage::field_projection(read(argv[2]));
            write(argv[3],okage::Bytes(reinterpret_cast<const std::uint8_t*>(p.data()),sizeof(p)));return 0;
        }
        if(command=="camera-angles" && argc==3) {
            const auto table=okage::make_atan_table();write(argv[2],okage::Bytes(reinterpret_cast<const std::uint8_t*>(table.data()),sizeof(table)));return 0;
        }
        if(command=="camera-look-at" && argc==4) {
            const auto input=read(argv[2]);if(input.empty() || input.size()%32)throw std::runtime_error("Look-at fixture bounds");
            const auto table=okage::make_atan_table();const auto sine=okage::make_sine_table();std::vector<okage::Float4> out;
            for(std::size_t at=0;at<input.size();at+=32) {
                okage::Float4 p,t;std::memcpy(p.data(),input.data()+at,16);std::memcpy(t.data(),input.data()+at+16,16);
                out.push_back(okage::camera_look_at(p,t,table,sine));
            }
            write(argv[3],okage::Bytes(reinterpret_cast<const std::uint8_t*>(out.data()),out.size()*16));return 0;
        }
        if(command=="dialogue-replay" && argc==8) {
            okage::DialogueText text(okage::read_pme(read(argv[2])).message(std::stoi(argv[3])));
            std::vector<okage::Font> fonts;
            for(const auto* name:{"IQ24.FNT","BMUI.FNT","BM.FNT"})fonts.push_back(okage::read_fnt(read(fs::path(argv[4])/name)));
            text.fonts=fonts;
            const auto trace=read(argv[5]),control=read(argv[6]);
            if(trace.empty() || trace.size()%160 || control.empty() || control.size()%512)throw std::runtime_error("Dialogue replay record bounds");
            std::array<std::uint32_t,40> r;std::memcpy(r.data(),trace.data(),160);
            if(r[1]!=0x1980f8)throw std::runtime_error("Dialogue replay initial text state");
            text.state.flags=r[10];text.state.progress=r[12];text.state.phase=r[16];text.state.period=r[17];text.state.delay=r[18];
            text.state.x=r[29];text.state.y=r[30];text.state.color=r[31];text.state.font=r[35];text.seek({r[13],r[14],r[15]!=0});
            okage::TextClock clock;clock.display_modes=r[11];clock.revealed=r[19];clock.scroll_target=__builtin_bit_cast(std::int32_t, (r[20]));
            clock.scroll=__builtin_bit_cast(float, (r[21]));clock.scroll_step=__builtin_bit_cast(float, (r[22]));clock.scroll_base_step=__builtin_bit_cast(float, (r[23]));clock.glyph_flags=r[32];
            std::array<std::uint32_t,64> c;std::memcpy(c.data(),control.data(),256);
            okage::DialogueController controller;controller.stage=c[7];controller.normal_period=__builtin_bit_cast(std::int32_t, (c[12]));
            controller.automatic=c[13]!=0;controller.window_type=__builtin_bit_cast(std::int32_t, (c[15]));
            controller.advance_count=c[16];controller.layer_flags=c[17];controller.layer_advance=c[21];
            if(c[18] || c[62])throw std::runtime_error("Dialogue replay choices outside scope");
            if((c[63]&255)==255)text.state.window.assign(1,char(255));
            else if(c[63]&255)throw std::runtime_error("Dialogue initial window command outside scope");
            okage::DialogueOpacity opacity;opacity.set(__builtin_bit_cast(float, (c[40])),__builtin_bit_cast(float, (c[43])),__builtin_bit_cast(float, (c[42])),__builtin_bit_cast(float, (c[41])));
            okage::DialogueWaitState wait{5,1};std::array<std::uint32_t,2> layers={c[22],c[23]};bool released=false;
            std::vector<std::uint32_t> result,wait_result;
            for(std::size_t at=0;at<control.size();at+=512) {
                std::memcpy(c.data(),control.data()+at,256);
                if(c[0]!=at/512 || c[1]!=0x148dc8)throw std::runtime_error("Dialogue input sequence mismatch");
                if(!released) {
                    const auto value=okage::poll_dialogue_wait(wait,2,{true,0,layers,__builtin_bit_cast(std::int32_t, (text.state.progress))});
                    wait_result.insert(wait_result.end(),{wait.countdown,wait.observed,wait.accumulated,__builtin_bit_cast(std::uint32_t, (value))});released=value==0;
                }
                opacity.tick();text.frame(clock,opacity.visible());
                const auto pos=text.position();
                result.insert(result.end(),{text.state.flags,text.state.progress,std::uint32_t(pos.fragment),std::uint32_t(pos.cursor),pos.loaded?1u:0u,
                    text.state.phase,text.state.period,text.state.delay,clock.revealed,__builtin_bit_cast(std::uint32_t, (clock.scroll_target)),__builtin_bit_cast(std::uint32_t, (clock.scroll)),text.state.x,text.state.y});
                const auto effects=controller.update(text,{std::uint16_t(c[8]),std::uint16_t(c[9])});
                if(effects.close)opacity.set(128,-4);
                result.insert(result.end(),{text.state.flags,text.state.phase,text.state.period,text.state.delay,text.state.progress,
                    controller.advance_count,controller.layer_advance,controller.layer_flags,controller.stage,__builtin_bit_cast(std::uint32_t, (opacity.delta))});
            }
            write(fs::path(argv[7])/"frames.bin",okage::Bytes(reinterpret_cast<const std::uint8_t*>(result.data()),result.size()*4));
            write(fs::path(argv[7])/"wait.bin",okage::Bytes(reinterpret_cast<const std::uint8_t*>(wait_result.data()),wait_result.size()*4));return 0;
        }
        if(command=="dialogue-close" && argc==5) {
            const auto input=read(argv[2]);
            if(input.size()!=40)throw std::runtime_error("Dialogue closing fixture requires ten words");
            std::array<std::uint32_t,10> row;std::memcpy(row.data(),input.data(),40);
            unsigned stage=row[0];okage::DialogueClosure closure;closure.message_flags=row[1];
            auto seed=[&](okage::DialogueOpacity& o,unsigned at){o.set(__builtin_bit_cast(float, (row[at])),__builtin_bit_cast(float, (row[at+3])),__builtin_bit_cast(float, (row[at+2])),__builtin_bit_cast(float, (row[at+1])));};
            seed(closure.decoration,2);okage::DialogueOpacity body;seed(body,6);
            const auto frames=std::stoul(argv[3]);if(frames>10000)throw std::runtime_error("Dialogue close frame limit");
            std::vector<std::uint32_t> output;
            for(unsigned frame=0;frame<=frames;++frame) {
                output.insert(output.end(),{stage,closure.message_flags});
                for(const auto& opacity:{closure.decoration,body})
                    for(float v:{opacity.value,opacity.upper,opacity.lower,opacity.delta})output.push_back(__builtin_bit_cast(std::uint32_t, (v)));
                if(frame<frames){closure.decoration.tick();body.tick();closure.update(stage);}
            }
            write(argv[4],okage::Bytes(reinterpret_cast<const std::uint8_t*>(output.data()),output.size()*4));return 0;
        }
        if(command=="dialogue-opacity" && argc==4) {
            const auto input=read(argv[2]);if(input.empty() || input.size()%16)throw std::runtime_error("Opacity fixture bounds");
            std::vector<float> out;
            for(std::size_t at=0;at<input.size();at+=16) {
                std::array<float,4> row;std::memcpy(row.data(),input.data()+at,16);
                okage::DialogueOpacity opacity;opacity.set(row[0],row[3],row[2],row[1]);opacity.tick();
                out.insert(out.end(),{opacity.value,opacity.upper,opacity.lower,opacity.delta});
            }
            write(argv[3],okage::Bytes(reinterpret_cast<const std::uint8_t*>(out.data()),out.size()*4));return 0;
        }
        if(command=="text-glyphs" && argc==5) {
            std::vector<okage::Font> fonts;
            for(const auto* name:{"IQ24.FNT","BMUI.FNT","BM.FNT"})fonts.push_back(okage::read_fnt(read(fs::path(argv[2])/name)));
            const auto source=read(argv[3]);
            if(source.empty() || source.size()%28)throw std::runtime_error("Glyph fixture bounds");
            std::vector<float> out;
            for(std::size_t at=0;at<source.size();at+=28) {
                std::array<std::uint32_t,7> r;std::memcpy(r.data(),source.data()+at,28);
                if(r[0]<32 || r[0]>=128)throw std::runtime_error("Glyph fixture encoding");
                okage::DialogueText text({std::string(1,char(r[0]))});text.fonts=fonts;
                text.state.flags=0x8001;text.state.period=0;text.state.font=r[1];text.state.color=r[2];text.state.x=r[3];text.state.y=r[4];
                text.seek({0,0,true});okage::TextClock clock;text.frame(clock);
                if(text.glyphs.size()!=1)throw std::runtime_error("Glyph fixture did not emit");
                const auto& g=text.glyphs[0];
                const auto c=g.color;
                const auto packed=((c&255)>>2)+((c>>8&255)>>2)*64+((c>>16&255)>>2)*4096+(c>>26)*262144;
                out.insert(out.end(),{float(g.x),float(g.y),float(packed),float(g.width),float(g.height),float(g.code),float(g.font)});
            }
            write(argv[4],okage::Bytes(reinterpret_cast<const std::uint8_t*>(out.data()),out.size()*4));return 0;
        }
        if((command=="font" || command=="font-metrics") && argc==4) {
            const auto font=okage::read_fnt(read(argv[2]));
            if(command=="font-metrics") {
                std::vector<std::uint8_t> out;
                for(const auto& g:font.glyphs)for(auto h:g.metrics) {out.push_back(h&255);out.push_back(h>>8);}
                write(argv[3],out);
            } else {
                const auto& t=font.atlas;
                const auto header="P7\nWIDTH "+std::to_string(t.width)+"\nHEIGHT "+std::to_string(t.height)+
                    "\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n";
                std::vector<std::uint8_t> out(header.begin(),header.end());out.insert(out.end(),t.rgba.begin(),t.rgba.end());
                write(argv[3],out);
            }
            std::cout<<unsigned(font.first)<<'\t'<<unsigned(font.last)<<'\t'<<font.atlas.width<<'\t'<<font.atlas.height<<'\n';return 0;
        }
        if(command=="text-clock" && argc==4) {
            const auto source=read(argv[2]);
            if(source.empty() || source.size()%40)throw std::runtime_error("Text clock fixture size");
            std::vector<std::uint32_t> output;
            for(std::size_t at=0;at<source.size();at+=40) {
                std::array<std::uint32_t,10> r;std::memcpy(r.data(),source.data()+at,40);
                okage::TextState state;state.flags=r[0];state.phase=r[2];state.period=r[3];state.delay=r[4];
                okage::TextClock clock;clock.display_modes=r[1];clock.revealed=r[5];
                clock.scroll_target=__builtin_bit_cast(std::int32_t, (r[6]));clock.scroll=__builtin_bit_cast(float, (r[7]));
                clock.scroll_step=__builtin_bit_cast(float, (r[8]));clock.scroll_base_step=__builtin_bit_cast(float, (r[9]));
                okage::tick_text_clock(state,clock);
                output.insert(output.end(),{state.flags,state.phase,state.period,state.delay,clock.revealed,__builtin_bit_cast(std::uint32_t, (clock.scroll))});
            }
            write(argv[3],okage::Bytes(reinterpret_cast<const std::uint8_t*>(output.data()),output.size()*4));return 0;
        }
        if((command=="text-frame-replay" && (argc==6 || argc==8)) || (command=="text-layout-replay" && argc==7)) {
            const bool layout=command=="text-layout-replay";
            okage::DialogueText text(okage::read_pme(read(argv[2])).message(std::stoi(argv[3])));
            std::vector<okage::Font> fonts;
            if(layout) {
                for(const auto* name:{"IQ24.FNT","BMUI.FNT","BM.FNT"})fonts.push_back(okage::read_fnt(read(fs::path(argv[5])/name)));
                text.fonts=fonts;
            }
            const auto input=read(argv[4]);
            if(input.empty() || input.size()%160)throw std::runtime_error("Text trace fixture size");
            okage::TextClock clock;bool initialized=false;
            std::vector<std::uint32_t> output;
            okage::DialogueWaitState wait{5,1};
            std::array<std::uint32_t,2> layers{};
            std::vector<std::uint32_t> wait_output;
            const auto request=argc==8 ? std::stoi(argv[6]) : 0;
            if(argc==8 && request<=0)throw std::runtime_error("Text frame wait needs positive signal count");
            bool released=false;
            for(std::size_t at=0;at<input.size();at+=160) {
                std::array<std::uint32_t,40> r;std::memcpy(r.data(),input.data()+at,160);
                if(!initialized) {
                    if(r[1]!=0x1980f8)throw std::runtime_error("Text replay must start at a clock entry");
                    text.state.flags=r[10];text.state.progress=r[12];text.state.phase=r[16];text.state.period=r[17];text.state.delay=r[18];
                    text.state.x=r[29];text.state.y=r[30];text.state.color=r[31];text.state.font=r[35];
                    text.seek({r[13],r[14],r[15]!=0});
                    clock.display_modes=r[11];clock.revealed=r[19];clock.scroll_target=__builtin_bit_cast(std::int32_t, (r[20]));
                    clock.scroll=__builtin_bit_cast(float, (r[21]));clock.scroll_step=__builtin_bit_cast(float, (r[22]));clock.scroll_base_step=__builtin_bit_cast(float, (r[23]));
                    clock.glyph_flags=r[32];initialized=true;
                    layers={r[7],r[8]};
                }
                if(r[1]==0x1985c0)text.resume();
                else if(r[1]==0x1a51a0)text.state.period=r[4];
                else if(r[1]==0x1980f8) {
                    if(argc==8 && !released) {
                        const auto result=okage::poll_dialogue_wait(wait,request,{true,0,layers,__builtin_bit_cast(std::int32_t, (text.state.progress))});
                        wait_output.insert(wait_output.end(),{wait.countdown,wait.observed,wait.accumulated,__builtin_bit_cast(std::uint32_t, (result))});
                        released=result==0;
                    }
                    text.frame(clock,r[24]!=0 || r[25]!=0);
                    const auto p=text.position();
                    output.insert(output.end(),{text.state.flags,text.state.progress,std::uint32_t(p.fragment),std::uint32_t(p.cursor),p.loaded ? 1u : 0u,
                        text.state.phase,text.state.period,text.state.delay,clock.revealed,__builtin_bit_cast(std::uint32_t, (clock.scroll_target)),__builtin_bit_cast(std::uint32_t, (clock.scroll))});
                    if(layout)output.insert(output.end(),{text.state.x,text.state.y});
                }
            }
            write(argv[layout ? 6 : 5],okage::Bytes(reinterpret_cast<const std::uint8_t*>(output.data()),output.size()*4));
            if(layout) {
                std::vector<std::uint32_t> glyphs;
                for(const auto& g:text.glyphs)glyphs.insert(glyphs.end(),{g.code,g.font,g.color,
                    __builtin_bit_cast(std::uint32_t, (g.x)),__builtin_bit_cast(std::uint32_t, (g.y)),std::uint32_t(g.width),std::uint32_t(g.height)});
                write(std::string(argv[6])+".glyphs.bin",okage::Bytes(reinterpret_cast<const std::uint8_t*>(glyphs.data()),glyphs.size()*4));
                const auto t=okage::rasterize_text(fonts,text.glyphs,640,224);
                const std::string header="P7\nWIDTH 640\nHEIGHT 224\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n";
                std::vector<std::uint8_t> pam(header.begin(),header.end());pam.insert(pam.end(),t.rgba.begin(),t.rgba.end());
                write(std::string(argv[6])+".pam",pam);
            }
            if(argc==8)write(argv[7],okage::Bytes(reinterpret_cast<const std::uint8_t*>(wait_output.data()),wait_output.size()*4));
            return 0;
        }
        if(command=="pad-repeat" && argc==4){
            const auto input=read(argv[2]);if(input.empty() || input.size()%4)throw std::runtime_error("Pad fixture requires held/pressed halfwords");
            okage::DialoguePadRepeat pad;std::vector<std::uint32_t> output;
            for(std::size_t i=0;i<input.size();i+=4){
                const auto repeat=pad.update(input[i]|(input[i+1]<<8),input[i+2]|(input[i+3]<<8));
                output.insert(output.end(),{repeat,pad.previous,pad.ticks});
            }
            write(argv[3],okage::Bytes(reinterpret_cast<const std::uint8_t*>(output.data()),output.size()*4));return 0;
        }
        if(command=="pme-branch" && argc==7) {
            const auto bank=okage::read_pme(read(argv[2]));auto path=bank.message_path(std::stoi(argv[3]));
            std::vector<std::uint32_t> output;
            auto snapshot=[&]{output.push_back(path.strings.size());for(unsigned i=0;i<path.strings.size();++i){output.push_back(path.strings[i]);output.push_back(path.nodes[i]);}};
            snapshot();bank.select_branch(path,std::stoul(argv[4]),std::stoul(argv[5]));snapshot();
            write(argv[6],okage::Bytes(reinterpret_cast<const std::uint8_t*>(output.data()),output.size()*4));return 0;
        }
        if(((command=="pme-message" || command=="pme-indices") && argc==5) || (command=="pme-name" && argc==6)) {
            auto bank=okage::read_pme(read(argv[2]));
            if(command=="pme-name" && !bank.bind("############",argv[4]))throw std::runtime_error("Missing player-name tag");
            std::vector<std::uint8_t> output;
            for(auto index:bank.message_indices(std::stoi(argv[3]))) {
                if(command=="pme-indices") {
                    for(unsigned i=0;i<4;++i)output.push_back(index>>(8*i));
                    continue;
                }
                const auto& text=bank.strings.at(index);
                const std::uint32_t size=text.size();
                for(unsigned i=0;i<4;++i)output.push_back(size>>(8*i));
                output.insert(output.end(),text.begin(),text.end());
            }
            write(argv[command=="pme-name" ? 5 : 4],output);return 0;
        }
        if(command=="pme-info" && argc==3) {
            const auto bank=okage::read_pme(read(argv[2]));
            std::cout<<bank.strings.size()<<" strings, "<<bank.nodes.size()<<" nodes\n";return 0;
        }
        if(command=="text-wait" && argc==6) {
            okage::DialogueText text(okage::read_pme(read(argv[2])).message(std::stoi(argv[3])));
            const auto request=std::stoi(argv[4]);
            if(request<=0)throw std::runtime_error("Text wait diagnostic requires a positive signal count");
            okage::DialogueWaitState wait{5,1};
            const std::array<std::uint32_t,2> layers{0x40,0x40};
            std::vector<std::uint32_t> output;
            for(unsigned step=0;step<100000;++step) {
                const auto kind=text.step();
                const auto result=okage::poll_dialogue_wait(wait,request,{true,0,layers,__builtin_bit_cast(std::int32_t, (text.state.progress))});
                output.insert(output.end(),{step,std::uint32_t(kind),text.state.progress,__builtin_bit_cast(std::uint32_t, (result)),
                                            wait.countdown,wait.observed,wait.accumulated});
                if(result==0) {
                    write(argv[5],okage::Bytes(reinterpret_cast<const std::uint8_t*>(output.data()),output.size()*4));
                    std::cout<<"Native text released event wait at semantic step "<<step<<" (not a frame count)\n";return 0;
                }
                if(kind==okage::TextStep::pause)text.resume();
                if(kind==okage::TextStep::end && !wait.countdown && wait.observed==text.state.progress)
                    throw std::runtime_error("Message ended before requested signal count");
            }
            throw std::runtime_error("Text wait diagnostic step limit");
        }
        if(command=="text-controls" && argc==4) {
            const auto bytes=read(argv[2]);
            const std::string text(bytes.begin(),bytes.end());
            okage::TextState state;state.x=5;state.y=7;state.line_origin=3;
            std::vector<std::uint8_t> output;
            auto word=[&](std::uint32_t n){for(unsigned i=0;i<4;++i)output.push_back(n>>(8*i));};
            std::size_t cursor=0;
            while(cursor<text.size()) {
                const auto result=okage::consume_text_control(text,cursor,state);
                for(auto n:{std::uint32_t(cursor),state.flags,state.progress,state.color,state.previous_color,
                    state.x,state.y,state.line_origin,state.line_height,state.font,state.period,state.phase,state.delay,result})word(n);
                for(const auto* value:{&state.speaker,&state.window,&state.option,&state.replacement,&state.description}) {
                    word(value->size());output.insert(output.end(),value->begin(),value->end());
                }
                if(!result)cursor+=static_cast<unsigned char>(text[cursor])>=0x80 ? 2 : 1;
            }
            write(argv[3],output);return 0;
        }
        if(command=="spu-sample" && argc==4) {
            const auto sample=okage::decode_spu_sample(read(argv[2]));
            write(argv[3],okage::Bytes(reinterpret_cast<const std::uint8_t*>(sample.pcm.data()),sample.pcm.size()*2));
            std::cout<<"Native SPU sample: "<<sample.pcm.size()<<" samples, "<<sample.consumed
                     <<" bytes, end flags "<<unsigned(sample.end_flags)<<", loop sample "
                     <<(sample.loop_sample ? std::to_string(*sample.loop_sample) : "none")<<'\n';return 0;
        }
        if(command=="dialogue-wait" && argc==4) {
            const auto cases=read(argv[2]);
            if(cases.empty() || cases.size()%48)throw std::runtime_error("Dialogue fixture record bounds");
            std::vector<std::uint32_t> output;
            for(std::size_t at=0;at<cases.size();at+=48) {
                std::array<std::uint32_t,12> in;std::memcpy(in.data(),cases.data()+at,48);
                if(in[8]>16 || (in[10]!=0 && in[10]!=0xffffffff) || in[6]>1)
                    throw std::runtime_error("Dialogue fixture input bounds");
                std::vector<std::uint32_t> layers;
                for(unsigned i=0;i<in[8];++i)layers.push_back((in[9]&(1u<<i)) ? 0x40 : 0);
                okage::DialogueWaitState state{in[1],in[2],in[3],in[4],in[5]};
                const okage::DialogueObservation dialogue{in[6]!=0,__builtin_bit_cast(std::int32_t, (in[7])),layers,
                    in[10]==0 ? __builtin_bit_cast(std::int32_t, (in[11])) : -1};
                const auto result=okage::poll_dialogue_wait(state,__builtin_bit_cast(std::int32_t, (in[0])),dialogue);
                output.insert(output.end(),{state.countdown,state.observed,state.accumulated,__builtin_bit_cast(std::uint32_t, (result))});
            }
            write(argv[3],okage::Bytes(reinterpret_cast<const std::uint8_t*>(output.data()),output.size()*4));return 0;
        }
        if(command=="event-setup" && argc==6) {
            const auto script=read(argv[2]),variables=read(argv[3]);auto state=read(argv[4]);
            const auto pcs=okage::run_event_setup(script,variables,state);
            write(std::string(argv[5])+".state.bin",state);
            write(std::string(argv[5])+".pcs.bin",okage::Bytes(reinterpret_cast<const std::uint8_t*>(pcs.data()),pcs.size()*4));
            std::cout<<"Native event setup completed "<<pcs.size()<<" instructions\n";return 0;
        }
        if(command=="snapshot-pose" && argc==5) {
            const auto model=okage::read_xmd(read(argv[2]));
            if(model.models.size()!=1)throw std::runtime_error("Snapshot requires one model");
            const auto bones=read(std::string(argv[3])+".bones.bin");
            const auto worldBytes=read(std::string(argv[3])+".world.bin"),viewBytes=read(std::string(argv[3])+".camera.bin");
            if(bones.size()!=model.models[0].bones.size()*48 || worldBytes.size()!=64 || viewBytes.size()!=48)
                throw std::runtime_error("Snapshot/model extent mismatch");
            okage::Matrix4 world;std::memcpy(world.data(),worldBytes.data(),64);
            okage::Float4 cameraPosition,cameraRotation,cameraScale;
            std::memcpy(cameraPosition.data(),viewBytes.data(),16);std::memcpy(cameraRotation.data(),viewBytes.data()+16,16);
            std::memcpy(cameraScale.data(),viewBytes.data()+32,16);
            const auto sine=okage::make_sine_table();
            const auto view=okage::camera_matrix(cameraPosition,cameraRotation,cameraScale,sine);
            std::vector<okage::Float4> channels(4+3*model.models[0].bones.size());
            std::memcpy(channels.data()+4,bones.data(),bones.size());
            const auto local=okage::animated_pose(model.models[0],channels,sine,true);
            const auto parent=okage::compose_pose(view,std::span(&world,1));
            const auto result=okage::compose_pose(parent[0],local);
            write(argv[4],okage::Bytes(reinterpret_cast<const std::uint8_t*>(result.data()),result.size()*64));return 0;
        }
        if ((command=="instance-matrices" || command=="camera-matrices") && argc==4) {
            const auto bytes=read(argv[2]);
            if(bytes.empty() || bytes.size()%48)throw std::runtime_error("Instance fixture requires 48-byte channel triples");
            std::vector<std::uint8_t> output;const auto sine=okage::make_sine_table();
            for(std::size_t at=0;at<bytes.size();at+=48) {
                okage::Float4 position,rotation,scale;
                std::memcpy(position.data(),bytes.data()+at,16);std::memcpy(rotation.data(),bytes.data()+at+16,16);
                std::memcpy(scale.data(),bytes.data()+at+32,16);
                const auto matrix=command=="camera-matrices" ? okage::camera_matrix(position,rotation,scale,sine) :
                    okage::instance_matrix(position,rotation,scale,sine);
                const auto* first=reinterpret_cast<const std::uint8_t*>(&matrix);output.insert(output.end(),first,first+64);
            }
            write(argv[3],output);return 0;
        }
        if ((command=="map" && argc==3) || (command=="map-transforms" && argc==4)) {
            const auto map=okage::read_xmp(read(argv[2]));
            if (command=="map") {
                std::cout<<"map\t"<<map.definitions.size()<<'\t'<<map.placements.size()<<'\n';
                for (std::size_t i=0;i<map.definitions.size();++i)
                    std::cout<<"definition\t"<<i<<'\t'<<map.definitions[i].name<<'\t'<<map.definitions[i].flags()<<'\n';
            }
            std::vector<std::uint8_t> result;
            auto append=[&](std::uint32_t word){for(unsigned i=0;i<4;++i) result.push_back(word>>(i*8));};
            for (std::size_t i=0;i<map.placements.size();++i) {
                const auto& p=map.placements[i];const auto t=okage::map_transform(p,map.definitions.at(p.definition()));
                if (command=="map") {
                    std::cout<<"placement\t"<<i<<'\t'<<p.definition()<<'\t'<<p.handler()<<'\t'<<t.flags;
                    for (const auto& v:{t.position,t.rotation,t.scale}) for (auto x:v) std::cout<<'\t'<<x;
                    std::cout<<'\n';
                }
                for (const auto& v:{t.position,t.rotation,t.scale}) for(auto x:v) append(__builtin_bit_cast(std::uint32_t, (x)));
                append(t.flags);
            }
            if (command=="map-transforms") write(argv[3],result);
            return 0;
        }
        if (command=="uv-model-cases" && argc==4) {
            const fs::path path=argv[2];
            const auto file=okage::read_xmd(read(path));
            std::vector<okage::Texture> images;
            for (auto name:file.textures) {
                if (fs::path(name).is_absolute() || fs::path(name).has_parent_path())
                    throw std::runtime_error("Unsafe model texture path");
                if (fs::path(name).extension().empty()) name+=".xim";
                images.push_back(okage::decode_xim(read(path.parent_path()/name)));
            }
            std::vector<std::uint8_t> cases;
            auto append=[&](std::uint32_t word) {for (unsigned i=0;i<4;++i) cases.push_back(word>>(i*8));};
            for (const auto& model:file.models) for (const auto& block:model.blocks) {
                std::vector<okage::Float4> coords(block.attribute_extents[3]);
                std::vector<bool> defined(coords.size());
                for (const auto& range:block.attributes) if (range.kind==8) {
                    for (std::size_t i=0;i<range.values.size();++i) {
                        if (defined.at(range.first+i)) throw std::runtime_error("Overlapping source UV ranges");
                        coords.at(range.first+i)=range.values[i]; defined.at(range.first+i)=true;
                    }
                }
                for (const auto& polygon:block.polygons) {
                    if (polygon.texture_state<0) continue;
                    const auto& state=block.textures.at(polygon.texture_state);
                    if (state.texture<0) continue;
                    const auto& image=images.at(state.texture);
                    for (const auto& corner:polygon.corners) {
                        if (corner[3]<0 || !defined.at(corner[3])) throw std::runtime_error("Missing polygon UV");
                        append(image.width); append(image.height);
                        append(std::uint32_t(std::int32_t(state.u_mode))); append(std::uint32_t(std::int32_t(state.v_mode)));
                        for (float value:state.region) append(__builtin_bit_cast(std::uint32_t, (value)));
                        append(__builtin_bit_cast(std::uint32_t, (coords.at(corner[3])[0])));
                        append(__builtin_bit_cast(std::uint32_t, (coords.at(corner[3])[1])));
                    }
                }
            }
            write(argv[3],cases); return 0;
        }
        if ((command=="uv-cases" || command=="texture-clamp-cases") && argc==4) {
            const auto cases=read(argv[2]);
            if (cases.empty() || cases.size()%40 || cases.size()>40*65536)
                throw std::runtime_error("Invalid UV fixture records");
            auto word=[&](std::size_t at) {
                return std::uint32_t(cases[at])|(std::uint32_t(cases[at+1])<<8)|
                    (std::uint32_t(cases[at+2])<<16)|(std::uint32_t(cases[at+3])<<24);
            };
            std::vector<std::uint8_t> result;
            for (std::size_t at=0;at<cases.size();at+=40) {
                const auto u=__builtin_bit_cast(std::int32_t, (word(at+8))),v=__builtin_bit_cast(std::int32_t, (word(at+12)));
                if (u<-32768 || u>32767 || v<-32768 || v>32767) throw std::runtime_error("UV fixture mode overflow");
                okage::TextureState state{0,0,std::int16_t(u),std::int16_t(v),{}};
                for (unsigned i=0;i<4;++i) state.region[i]=__builtin_bit_cast(float, (word(at+16+i*4)));
                if (command=="texture-clamp-cases") {
                    const auto bits=okage::texture_clamp(state,word(at),word(at+4));
                    for (unsigned i=0;i<8;++i) result.push_back(bits>>(i*8));
                    continue;
                }
                const auto uv=okage::texture_coordinates(state,word(at),word(at+4),
                    {__builtin_bit_cast(float, (word(at+32))),__builtin_bit_cast(float, (word(at+36)))});
                for (float value:uv) {
                    auto bits=__builtin_bit_cast(std::uint32_t, (value));
                    for (unsigned i=0;i<4;++i) result.push_back(bits>>(i*8));
                }
            }
            write(argv[3],result); return 0;
        }
        if (command=="texture-callback" && argc==5) {
            const auto model=okage::read_xmd(read(argv[2]));
            const auto text=read(argv[3]);
            okage::TextureBindings textures(model,okage::read_xta(std::string_view(reinterpret_cast<const char*>(text.data()),text.size())));
            std::vector<std::int32_t> dump;
            for (std::size_t group=0;group<textures.animation().groups.size();++group)
                for (std::size_t variant=0;variant<=textures.animation().variants.size();++variant) {
                    textures.restore(); textures.select(group,variant);
                    dump.push_back(group); dump.push_back(variant);
                    dump.insert(dump.end(),textures.images().begin(),textures.images().end());
                }
            write(argv[4],okage::Bytes(reinterpret_cast<const std::uint8_t*>(dump.data()),dump.size()*4)); return 0;
        }
        if (command == "animation" && (argc >= 7 && argc <= 9)) {
            const auto count=std::stoul(argv[3]), entry=std::stoul(argv[4]), frames=std::stoul(argv[5]);
            if (count>5120 || frames>10000 || (frames+1)*(8+count*16)>32*1024*1024)
                throw std::runtime_error("Animation dump exceeds bounds");
            okage::Animation animation(read(argv[2]),count);
            okage::ModelFile model;
            const auto sine=okage::make_sine_table();
            if (argc>=8) {
                model=okage::read_xmd(read(argv[7]));
                if (model.models.size()!=1 || (frames+1)*(8+count*16+model.models[0].hierarchy.size()*64)>32*1024*1024)
                    throw std::runtime_error("Animation pose dump exceeds bounds");
            }
            std::unique_ptr<okage::TextureBindings> textures;
            if (argc==9) {
                const auto text=read(argv[8]);
                textures=std::make_unique<okage::TextureBindings>(model,okage::read_xta(
                    std::string_view(reinterpret_cast<const char*>(text.data()),text.size())));
                if ((frames+1)*(8+count*16+model.models[0].hierarchy.size()*64+textures->images().size()*4)>32*1024*1024)
                    throw std::runtime_error("Texture animation dump exceeds bounds");
                animation.set_texture_callback([&](std::size_t group,std::size_t variant) {textures->select(group,variant);});
            }
            animation.start(0); animation.values(); animation.start(entry);
            std::vector<std::uint8_t> dump;
            auto append=[&](const void* data,std::size_t size) {
                auto* p=static_cast<const std::uint8_t*>(data); dump.insert(dump.end(),p,p+size);
            };
            for (std::size_t frame=0;frame<=frames;++frame) {
                if (frame) animation.tick();
                const std::uint32_t state[]={animation.pc(),animation.script_state()};
                append(state,sizeof(state));
                const auto values=animation.values(); append(values.data(),values.size()*sizeof(okage::Float4));
                if (argc>=8) {
                    const auto pose=okage::animated_pose(model.models[0],values,sine,animation.scale_channels());
                    append(pose.data(),pose.size()*sizeof(okage::Matrix4));
                }
                if (textures) append(textures->images().data(),textures->images().size()*4);
            }
            write(argv[6],dump); return 0;
        }
        if (command == "sine-table" && argc == 3) {
            const auto table=okage::make_sine_table();
            write(argv[2],okage::Bytes(reinterpret_cast<const std::uint8_t*>(table.data()),sizeof(table)));
            return 0;
        }
        if (command == "pose" && argc == 4) {
            const auto file=okage::read_xmd(read(argv[2]));
            if (file.models.size()!=1) throw std::runtime_error("Pose dump expects one model");
            const auto pose=okage::rest_pose(file.models[0],okage::make_sine_table());
            write(argv[3],okage::Bytes(reinterpret_cast<const std::uint8_t*>(pose.data()),pose.size()*sizeof(okage::Matrix4)));
            return 0;
        }
        if (command == "pose-corpus" && argc == 4) {
            std::vector<fs::path> files;
            for (const auto& entry : fs::recursive_directory_iterator(argv[2]))
                if (entry.is_regular_file() && entry.path().extension()==".xmd") files.push_back(entry.path());
            std::sort(files.begin(),files.end());
            if (files.empty()) throw std::runtime_error("No XMD models found");
            const auto sine=okage::make_sine_table();
            for (const auto& path: files) {
                const auto model=okage::read_xmd(read(path));
                if (model.models.size()!=1) throw std::runtime_error("Pose corpus expects one model per file");
                const auto pose=okage::rest_pose(model.models[0],sine);
                auto output=fs::path(argv[3])/fs::relative(path,argv[2]); output.replace_extension(".pose");
                write(output,okage::Bytes(reinterpret_cast<const std::uint8_t*>(pose.data()),pose.size()*sizeof(okage::Matrix4)));
                std::cout<<path.string()<<'\t'<<output.string()<<'\t'<<pose.size()<<'\n';
            }
            return 0;
        }
        if (command == "model" && argc == 3) { model_report(argv[2]); return 0; }
        if (command == "model-check" && argc == 3) {
            std::vector<fs::path> files;
            for (const auto& entry : fs::recursive_directory_iterator(argv[2])) {
                auto extension=entry.path().extension().string();
                if (entry.is_regular_file() && (extension==".xmd" || extension==".XMD")) files.push_back(entry.path());
            }
            std::sort(files.begin(),files.end());
            if (files.empty()) throw std::runtime_error("No XMD models found");
            for (const auto& path : files) {
                try { model_report(path); }
                catch (const std::exception& e) { throw std::runtime_error(path.string()+": "+e.what()); }
            }
            return 0;
        }
        if (command == "texture-check" && argc == 3) {
            std::vector<fs::path> files;
            for (const auto& entry : fs::recursive_directory_iterator(argv[2])) {
                auto extension = entry.path().extension().string();
                if (entry.is_regular_file() && (extension == ".xim" || extension == ".XIM")) files.push_back(entry.path());
            }
            std::sort(files.begin(), files.end());
            for (const auto& path : files) {
                try {
                    const auto texture = okage::decode_xim(read(path));
                    std::cout << path.string() << '\t' << texture.width << '\t' << texture.height << '\t'
                              << texture.pixel_format << '\t' << texture.palette_format << '\n';
                } catch (const std::exception& e) { throw std::runtime_error(path.string() + ": " + e.what()); }
            }
            return 0;
        }
        const auto bytes = read(argv[2]);
        if (command == "texture" && argc == 4) {
            const auto texture = okage::decode_xim(bytes);
            const auto header = "P7\nWIDTH " + std::to_string(texture.width) + "\nHEIGHT " + std::to_string(texture.height) +
                                "\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n";
            std::vector<std::uint8_t> pam(header.begin(), header.end());
            pam.insert(pam.end(), texture.rgba.begin(), texture.rgba.end());
            write(argv[3], pam);
            std::cout << texture.width << '\t' << texture.height << '\t' << texture.pixel_format << '\t' << texture.palette_format << '\n';
        } else if (command == "decode" && argc == 4) {
            auto result = okage::decompress_xpf(bytes);
            write(argv[3], result.bytes);
            std::cout << result.bytes.size() << '\t' << result.consumed << '\n';
        } else if ((command == "list" && argc == 3) || (command == "extract" && argc == 4)) {
            const auto entries = okage::read_xpf(bytes);
            for (const auto& entry : entries) {
                auto result = okage::decompress_xpf(okage::Bytes(bytes).subspan(entry.offset, entry.packed_size));
                if (command == "extract") write(fs::path(argv[3]) / entry.name, result.bytes);
                std::cout << entry.name << '\t' << entry.offset << '\t' << entry.packed_size
                          << '\t' << result.bytes.size() << '\t' << result.consumed << '\n';
            }
        } else throw std::runtime_error("Invalid command or arguments");
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "okage-assets: " << e.what() << '\n';
        return 1;
    }
}
