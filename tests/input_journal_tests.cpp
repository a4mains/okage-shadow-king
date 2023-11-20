#include "okage/input_journal.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace {
void require(bool value) { if (!value) throw std::runtime_error("input journal assertion failed"); }

template<class F> void rejects(F&& fn) {
    bool failed = false;
    try { fn(); } catch (const std::exception&) { failed = true; }
    require(failed);
}

struct TempDir {
    std::filesystem::path path;
    TempDir() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() /
               ("okage-input-journal-" + std::to_string(::getpid()) + "-" + std::to_string(stamp));
        std::filesystem::create_directory(path);
    }
    ~TempDir() { std::error_code ec; std::filesystem::remove_all(path, ec); }
};

std::string read(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void raw_inputs_and_lifecycle() {
    TempDir temp;
    const auto path = temp.path / "inputs.jsonl";
    {
        okage::InputJournal journal(path);
        require(journal.frame() == 0);
        journal.begin_tick(40, 0, 4); // A fast press edge need not be held.
        require(journal.frame() == 40);
        journal.complete_tick(40);
        require(journal.frame() == 40);
        journal.pause(40, {.scene = 12, .event_pc = 99});
        require(journal.tick_pending() == false && !journal.terminal());
        // Explicit pause flush makes the prefix visible while the writer remains alive.
        const auto prefix = read(path);
        require(prefix.find("\"kind\":\"pause\",\"frame\":40,\"scene\":12,\"event_pc\":99") != std::string::npos);
        journal.resume(40);
        journal.begin_tick(41, 4, 4);
        journal.complete_tick(41);
        journal.begin_tick(42, 0, 4);
        journal.complete_tick(42);
        journal.finish(42);
        require(journal.terminal());
        rejects([&] { journal.finish(42); });
    }
    const auto text = read(path);
    require(text.find("\"kind\":\"tick_attempt\",\"frame\":40,\"held\":0,\"pressed\":4") != std::string::npos);
    require(text.find("\"kind\":\"tick_attempt\",\"frame\":41,\"held\":4,\"pressed\":4") != std::string::npos);
    require(text.find("\"kind\":\"tick_attempt\",\"frame\":42,\"held\":0,\"pressed\":4") != std::string::npos);
    require(text.find("\"kind\":\"finish\",\"frame\":42") != std::string::npos);
    const auto pause = text.find("\"kind\":\"pause\"");
    const auto event_pc = text.find("\"event_pc\"");
    require(pause != std::string::npos && event_pc > pause);
}

void failed_tick_is_distinct() {
    TempDir temp;
    const auto path = temp.path / "failed.jsonl";
    okage::InputJournal journal(path);
    journal.begin_tick(7, 0x12, 0x80);
    journal.error(7, "tick failed: \"bad\"\ntrace", {.scene = 3});
    require(journal.terminal());
    const auto text = read(path);
    require(text.find("\"kind\":\"tick_attempt\",\"frame\":7,\"held\":18,\"pressed\":128") != std::string::npos);
    require(text.find("\"kind\":\"tick_complete\"") == std::string::npos);
    require(text.find("\"kind\":\"error\",\"frame\":7,\"scene\":3,\"message\":\"tick failed: \\\"bad\\\"\\ntrace\",\"attempted_tick_frame\":7,\"tick_status\":\"failed\"") != std::string::npos);
}

void rejects_invalid_order_and_lifecycle() {
    TempDir temp;
    okage::InputJournal journal(temp.path / "ordered.jsonl");
    rejects([&] { journal.complete_tick(1); });
    journal.begin_tick(5, 1, 2);
    rejects([&] { journal.begin_tick(6, 0, 1); });
    rejects([&] { journal.complete_tick(6); });
    rejects([&] { journal.pause(5); });
    journal.complete_tick(5);
    rejects([&] { journal.begin_tick(5, 0, 0); });
    journal.pause(5);
    rejects([&] { journal.pause(5); });
    rejects([&] { journal.begin_tick(6, 0, 0); });
    journal.resume(5);
    journal.begin_tick(6, 0, 0);
    journal.complete_tick(6);
    journal.finish(6);
}

void refuses_existing_file() {
    TempDir temp;
    const auto path = temp.path / "existing.jsonl";
    { std::ofstream out(path); out << "keep me"; }
    rejects([&] { okage::InputJournal journal(path); });
    require(read(path) == "keep me");
}

} // namespace

int main() {
    try {
        raw_inputs_and_lifecycle();
        failed_tick_is_distinct();
        rejects_invalid_order_and_lifecycle();
        refuses_existing_file();
        std::cout << "input journal tests passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
