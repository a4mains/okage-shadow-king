#include "okage/input_journal.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

namespace okage {
namespace {

std::string json_string(std::string_view value) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('"');
    for (const unsigned char ch : value) {
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (ch < 0x20) {
                    out += "\\u00";
                    out.push_back(hex[ch >> 4]);
                    out.push_back(hex[ch & 0x0f]);
                } else {
                    out.push_back(static_cast<char>(ch));
                }
        }
    }
    out.push_back('"');
    return out;
}

} // namespace

struct InputJournal::Impl {
    enum class State { active, paused, terminal };

    std::FILE* file = nullptr;
    State state = State::active;
    std::optional<std::uint64_t> last_frame;
    std::optional<std::uint64_t> pending_frame;
    bool poisoned = false;

    explicit Impl(const std::filesystem::path& path) {
        const std::string native_path = path.string();
        const int fd = ::open(native_path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd < 0) throw std::system_error(errno, std::generic_category(), "create input journal");
        file = ::fdopen(fd, "w");
        if (!file) {
            const int error = errno;
            ::close(fd);
            throw std::system_error(error, std::generic_category(), "open input journal stream");
        }
        try {
            write_line("{\"kind\":\"header\",\"schema\":\"okage-native-input-journal-v1\",\"version\":1}");
            flush();
        } catch (...) {
            std::fclose(file);
            file = nullptr;
            throw;
        }
    }

    ~Impl() {
        if (file) std::fclose(file);
    }

    void ensure_usable() const {
        if (poisoned) throw std::runtime_error("input journal is unusable after a write failure");
    }

    void write_line(const std::string& line) {
        ensure_usable();
        if (std::fwrite(line.data(), 1, line.size(), file) != line.size() || std::fputc('\n', file) == EOF) {
            poisoned = true;
            throw std::system_error(errno ? errno : EIO, std::generic_category(), "write input journal");
        }
    }

    void flush() {
        ensure_usable();
        if (std::fflush(file) != 0) {
            poisoned = true;
            throw std::system_error(errno ? errno : EIO, std::generic_category(), "flush input journal");
        }
    }

    void check_frame(std::uint64_t frame, bool strictly_after) const {
        ensure_usable();
        if (last_frame && (strictly_after ? frame <= *last_frame : frame < *last_frame)) {
            throw std::invalid_argument(strictly_after ? "input journal frames must increase" :
                                                          "input journal frame moved backwards");
        }
    }

    void require_nonterminal() const {
        ensure_usable();
        if (state == State::terminal) throw std::logic_error("input journal is terminal");
    }

    void add_context(std::string& line, const InputJournalContext& context) {
        if (context.scene) line += ",\"scene\":" + std::to_string(*context.scene);
        if (context.event_pc) line += ",\"event_pc\":" + std::to_string(*context.event_pc);
    }

    void marker(std::string_view kind, std::uint64_t frame, const InputJournalContext& context,
                std::optional<std::string_view> message = std::nullopt) {
        std::string line = "{\"kind\":" + json_string(kind) + ",\"frame\":" + std::to_string(frame);
        add_context(line, context);
        if (message) line += ",\"message\":" + json_string(*message);
        if (pending_frame) {
            line += ",\"attempted_tick_frame\":" + std::to_string(*pending_frame);
            if (kind == "error") line += ",\"tick_status\":\"failed\"";
        }
        line += '}';
        write_line(line);
        flush();
        last_frame = frame;
        if (kind == "pause") state = State::paused;
        else if (kind == "resume") state = State::active;
        else state = State::terminal;
        if (kind == "error" || kind == "finish") pending_frame.reset();
        if (state == State::terminal) {
            if (std::fclose(file) != 0) {
                file = nullptr;
                poisoned = true;
                throw std::system_error(errno ? errno : EIO, std::generic_category(), "close input journal");
            }
            file = nullptr;
        }
    }
};

InputJournal::InputJournal(const std::filesystem::path& path) : impl_(std::make_unique<Impl>(path)) {}
InputJournal::~InputJournal() = default;

void InputJournal::begin_tick(std::uint64_t frame, std::uint32_t held, std::uint32_t pressed) {
    auto& p = *impl_;
    p.require_nonterminal();
    if (p.state != Impl::State::active) throw std::logic_error("cannot tick while input journal is paused");
    if (p.pending_frame) throw std::logic_error("input journal already has a pending tick");
    p.check_frame(frame, true);
    p.write_line("{\"kind\":\"tick_attempt\",\"frame\":" + std::to_string(frame) +
                 ",\"held\":" + std::to_string(held) + ",\"pressed\":" + std::to_string(pressed) + "}");
    p.pending_frame = frame;
}

void InputJournal::complete_tick(std::uint64_t frame) {
    auto& p = *impl_;
    p.require_nonterminal();
    if (p.state != Impl::State::active || !p.pending_frame) throw std::logic_error("no active tick to complete");
    if (frame != *p.pending_frame) throw std::invalid_argument("completed tick frame does not match attempted frame");
    p.write_line("{\"kind\":\"tick_complete\",\"frame\":" + std::to_string(frame) + "}");
    p.last_frame = frame;
    p.pending_frame.reset();
}

void InputJournal::pause(std::uint64_t frame, InputJournalContext context) {
    auto& p = *impl_; p.require_nonterminal();
    if (p.state != Impl::State::active || p.pending_frame) throw std::logic_error("cannot pause input journal in current state");
    p.check_frame(frame, false); p.marker("pause", frame, context);
}

void InputJournal::resume(std::uint64_t frame, InputJournalContext context) {
    auto& p = *impl_; p.require_nonterminal();
    if (p.state != Impl::State::paused) throw std::logic_error("input journal is not paused");
    p.check_frame(frame, false); p.marker("resume", frame, context);
}

void InputJournal::error(std::uint64_t frame, std::string_view message, InputJournalContext context) {
    auto& p = *impl_; p.require_nonterminal();
    if (p.state == Impl::State::active && p.pending_frame && frame != *p.pending_frame)
        throw std::invalid_argument("error frame must match pending tick frame");
    p.check_frame(frame, false); p.marker("error", frame, context, message);
}

void InputJournal::finish(std::uint64_t frame, InputJournalContext context) {
    auto& p = *impl_; p.require_nonterminal();
    if (p.pending_frame) throw std::logic_error("cannot finish with an uncommitted tick");
    p.check_frame(frame, false); p.marker("finish", frame, context);
}

bool InputJournal::terminal() const noexcept { return impl_->state == Impl::State::terminal; }
bool InputJournal::tick_pending() const noexcept { return impl_->pending_frame.has_value(); }
std::uint64_t InputJournal::frame() const noexcept {
    if (impl_->pending_frame) return *impl_->pending_frame;
    return impl_->last_frame.value_or(0);
}

} // namespace okage
