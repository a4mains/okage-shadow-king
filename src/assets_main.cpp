#include "okage/assets.hpp"
#include "okage/model.hpp"
#include "okage/pose.hpp"
#include "okage/animation.hpp"
#include "okage/texture_animation.hpp"
#include "okage/render.hpp"
#include <algorithm>
#include <bit>
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
