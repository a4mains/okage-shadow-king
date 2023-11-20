#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>

namespace okage {

struct InputJournalContext {
    std::optional<std::uint32_t> scene;
    std::optional<std::uint32_t> event_pc;
};

class InputJournal {
public:
    explicit InputJournal(const std::filesystem::path& path);
    ~InputJournal();

    InputJournal(const InputJournal&) = delete;
    InputJournal& operator=(const InputJournal&) = delete;
    InputJournal(InputJournal&&) = delete;
    InputJournal& operator=(InputJournal&&) = delete;

    void begin_tick(std::uint64_t frame, std::uint32_t held, std::uint32_t pressed);
    void complete_tick(std::uint64_t frame);

    void pause(std::uint64_t frame, InputJournalContext context = {});
    void resume(std::uint64_t frame, InputJournalContext context = {});
    void error(std::uint64_t frame, std::string_view message, InputJournalContext context = {});
    void finish(std::uint64_t frame, InputJournalContext context = {});

    [[nodiscard]] bool terminal() const noexcept;
    [[nodiscard]] bool tick_pending() const noexcept;
    [[nodiscard]] std::uint64_t frame() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace okage
