#include "restore_session.hpp"
#include "okage/font.hpp"
#include "okage/game_mode.hpp"
#include "okage/save_store.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <thread>
#include <unistd.h>

namespace {
using namespace okage;
namespace fs = std::filesystem;
void check(bool value, const std::source_location at = std::source_location::current()) {
    if (!value) throw std::runtime_error("Restore publication assertion line " + std::to_string(at.line()));
}
std::vector<std::uint8_t> read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read test asset " + path.string());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
void put(SavePayload& bytes, unsigned at, unsigned value) {
    for (unsigned i = 0; i < 4; ++i) bytes.at(at + i) = value >> (8 * i);
}
struct TempDir {
    fs::path path;
    TempDir() {
        std::string pattern = (fs::temp_directory_path() / "okage-restore-publication-XXXXXX").string();
        const auto result = ::mkdtemp(pattern.data());
        if (!result) throw std::runtime_error("Cannot create Restore test directory");
        path = result;
    }
    ~TempDir() { std::error_code ignored; fs::remove_all(path, ignored); }
};
SavePayload slot_payload() {
    SavePayload payload{};
    put(payload, 0x510, 74); put(payload, 0x514, 1);
    put(payload, 0x51c, 123); put(payload, 0x520, 1234);
    payload[0x5c6] = 0xaa;
    payload[0x5c8] = 1; put(payload, 0x5cc, 20); payload[0x5dc] = 32;
    std::copy_n("Ari", 4, payload.begin() + 0x844);
    std::copy_n("Own Room", 9, payload.begin() + 0x864);
    return payload;
}
struct Assets {
    fs::path common, ui;
    std::shared_ptr<const std::vector<Font>> fonts;
    GameDataTables tables;
    Assets(const fs::path& common_root, const fs::path& ui_root,
           const fs::path& font_root, const fs::path& data_root)
        :common(common_root), ui(ui_root), tables(read(data_root / "BKDATA.XDB"), read(data_root / "ITEM.XDB")) {
        std::vector<Font> loaded;
        for (const auto name : {"IQ24.FNT", "BMUI.FNT", "BM.FNT"})
            loaded.push_back(read_fnt(read(font_root / name)));
        fonts = std::make_shared<const std::vector<Font>>(std::move(loaded));
    }
};
void route(const Assets& assets, bool populated, bool load, bool observe_publication) {
    TempDir temp;
    const auto payload = slot_payload();
    NativeSaveStore store(temp.path);
    if (populated) store.write(0, payload);
    const auto file_before = populated ? read(temp.path / "bkmo0.dat") : std::vector<std::uint8_t>{};
    SceneClearColor clear; SceneFade fade; BattleUiShared ui; RestoreShared shared;
    unsigned stopped = 0;
    RestoreConstruction construction;
    construction.common_root = assets.common; construction.ui_root = assets.ui;
    construction.save_directory = temp.path; construction.fonts = assets.fonts;
    construction.stop_music = [&] { ++stopped; };
    RestoreSession session(clear, fade, ui, shared, std::move(construction));
    check(stopped == 1 && session.resource_live());
    Animation camera(3); reset_scene_camera(camera); FieldProjection projection;
    GameRandom random{1234}; const auto sine = make_sine_table(); unsigned image_counter = 42;
    std::vector<std::uint8_t> variables(0x1000);
    GameModeParent parent({10, 0, 10});
    unsigned tick = 0, publications = 0, requests = 0, publication_tick = 0, request_tick = 0;
    unsigned death_tick = 0, destruction_tick = 0, free_tick = 0, construction_tick = 0;
    std::uint32_t death_rng = 0;
    const GameModeSelection expected = load ? GameModeSelection{2, 74, 1} : GameModeSelection{8, 1, 0};
    auto step = [&](BattleUiPadInput input = {}) {
        ++tick;
        // The wrapper's parent runs before its resource/manager descendants.
        parent.tick([&] {
            destruction_tick = tick;
            session.retire(camera, projection, nullptr, ui);
        }, [&] { free_tick = tick; }, [&](auto selection) {
            check(selection == expected); construction_tick = tick;
        });
        if (!session.resource_live()) return;
        const bool was_dead = session.dead();
        if (shared.state == -1) shared.alpha.fill(30);
        const auto before_publications = publications;
        const auto finish = [&](bool loaded) {
            check(loaded == load && session.dead());
            check(death_tick && tick == death_tick + 2);
            check(publications == (observe_publication ? 1u : 0u));
            ++requests; request_tick = tick; parent.request(expected);
        };
        const auto publish = [&](bool loaded) {
            check(loaded == load && session.dead() && !was_dead);
            check(session.last_frame().manager_ran && session.last_frame().state_out == 0);
            check(shared.alpha.at(shared.selected) == 30); // Before11c2c8, not after it.
            check(!parent.requested() && requests == 0 && publications == 0);
            ++publications; publication_tick = tick;
        };
        const auto sound = [](unsigned id) { check(id == 3 || id == 4 || id == 7); };
        if (observe_publication)
            session.tick(input, variables, assets.tables, fade, camera, projection, nullptr, ui,
                         image_counter, sine, random, sound, finish, publish);
        else
            session.tick(input, variables, assets.tables, fade, camera, projection, nullptr, ui,
                         image_counter, sine, random, sound, finish);
        if (!was_dead && session.dead()) {
            death_tick = tick; death_rng = random.state;
            check(!parent.requested() && requests == 0);
            check(publications == (observe_publication ? 1u : 0u));
        }
        if (publications != before_publications)
            check(shared.alpha.at(shared.selected) == 34); // Second callback still ran on D.
        if (death_tick && tick > death_tick) {
            check(!session.last_frame().manager_ran && random.state == death_rng);
            check(publications == (observe_publication ? 1u : 0u));
        }
        check(image_counter == 42);
        std::this_thread::yield();
    };
    auto until = [&](auto condition) {
        for (unsigned i = 0; i < 2000 && !condition(); ++i) step();
        check(condition());
    };
    until([&] { return shared.state == (populated ? 550 : 540) && session.last_frame().gate_open; });
    check(session.saved_slots_present() == populated);
    if (load) {
        step({0x40, 0x40}); check(shared.state == 560);
        step({0, 0, 0, 0x8000}); // Actual Yes selection; initially No.
        step({0x40, 0x40}); check(shared.state == 700);
        until([&] { return shared.state == -1; });
        check(std::equal(payload.begin(), payload.end(), variables.begin()));
    } else {
        step({0x20, 0x20}); check(shared.state == -1);
    }
    until([&] { return death_tick != 0; });
    if (observe_publication) check(publication_tick == death_tick);
    step(); check(requests == 0 && !parent.requested()); // D+1 manager unlink.
    step(); check(requests == 1 && request_tick == death_tick + 2 && parent.requested() == expected);
    step(); check(destruction_tick == death_tick + 3 && !session.resource_live());
    step(); check(free_tick == death_tick + 4 && !construction_tick);
    step(); check(construction_tick == death_tick + 5);
    check(publications == (observe_publication ? 1u : 0u) && requests == 1 && random.state == death_rng);
    if (!load) check(std::all_of(variables.begin(), variables.end(), [](auto value) { return value == 0; }));
    if (populated) check(read(temp.path / "bkmo0.dat") == file_before && store.read(0) == payload);
    else check(!store.read(0));
}
}  // namespace

int main(int argc, char** argv) { @autoreleasepool { try {
    if (argc != 5) throw std::runtime_error("Usage: restore_session_tests COMMON_ROOT UI_ROOT FONT_DIRECTORY XDB_DIRECTORY");
    const Assets assets(argv[1], argv[2], argv[3], argv[4]);
    route(assets, true, true, true);
    route(assets, true, false, true);
    route(assets, false, false, true);
    route(assets, true, true, false);
    std::cout << "Restore publication, saved read, Cancel and wrapper lifecycle tests passed\n";
    return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; } } }
