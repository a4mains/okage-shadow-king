#include "okage/pcm_stream.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace okage {
namespace {
std::uint32_t read_u32(Bytes bytes, std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 4)
        throw std::runtime_error("Title XWH word bounds");
    return std::uint32_t(bytes[offset]) |
           (std::uint32_t(bytes[offset + 1]) << 8) |
           (std::uint32_t(bytes[offset + 2]) << 16) |
           (std::uint32_t(bytes[offset + 3]) << 24);
}

std::uint16_t read_u16(Bytes bytes, std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 2)
        throw std::runtime_error("Title XWH halfword bounds");
    return std::uint16_t(bytes[offset]) |
           (std::uint16_t(bytes[offset + 1]) << 8);
}

void require_tag(Bytes bytes, std::size_t offset, std::string_view tag) {
    if (offset > bytes.size() || bytes.size() - offset < tag.size() ||
        !std::equal(tag.begin(), tag.end(), bytes.begin() + offset))
        throw std::runtime_error("Unsupported title XWH tag");
}

struct ParsedRecord {
    std::uint32_t body_base = 0;
    std::uint32_t body_end = 0;
    std::uint32_t loop = 0;
    int left_gain = 0;
    int right_gain = 0;
};

ParsedRecord parse_xwh(Bytes xwh) {
    if (xwh.size() < 16 || read_u32(xwh, 4) != xwh.size() - 16 ||
        read_u32(xwh, 8) != 0x200 || read_u32(xwh, 12) != 0)
        throw std::runtime_error("Unsupported title RXWS header/extent");
    require_tag(xwh, 0, "RXWS");

    std::size_t at = 16;
    if (xwh.size() - at < 16) throw std::runtime_error("Title FORM header extent");
    require_tag(xwh, at, "FORM");
    const auto form_size = read_u32(xwh, at + 4);
    if (read_u32(xwh, at + 8) != 0x100 || read_u32(xwh, at + 12) != 0 ||
        form_size > xwh.size() - at - 16)
        throw std::runtime_error("Unsupported title FORM version/extent");
    const auto form_begin = at + 16;
    const auto form_end = form_begin + form_size;
    if (form_size != 32 || read_u32(xwh, form_begin) != 1)
        throw std::runtime_error("Unsupported title FORM record count/extent");

    const auto record = form_begin + 4;
    if (xwh[record] != 1 || xwh[record + 1] != 0x1c ||
        xwh[record + 2] != 0x03 || xwh[record + 9] != 2 ||
        read_u16(xwh, record + 10) != TitlePcmStream::sample_rate)
        throw std::runtime_error("Unsupported title format-1 record");

    ParsedRecord parsed;
    parsed.left_gain = std::bit_cast<std::int8_t>(xwh[record + 4]);
    parsed.right_gain = std::bit_cast<std::int8_t>(xwh[record + 5]);
    parsed.body_base = read_u32(xwh, record + 16);
    parsed.body_end = read_u32(xwh, record + 20);
    parsed.loop = read_u32(xwh, record + 24);
    if (parsed.body_base != 0 || parsed.body_end == 0 ||
        parsed.loop <= parsed.body_base || parsed.loop >= parsed.body_end ||
        (parsed.body_end - parsed.body_base) % 4 != 0 ||
        (parsed.loop - parsed.body_base) % 4 != 0)
        throw std::runtime_error("Unsupported title XWB body/loop extent");
    if (form_end != at + 16 + form_size)
        throw std::runtime_error("Title FORM trailing bytes");

    at = form_end;
    if (xwh.size() - at < 16) throw std::runtime_error("Title FTXT header extent");
    require_tag(xwh, at, "FTXT");
    const auto text_size = read_u32(xwh, at + 4);
    if (read_u32(xwh, at + 8) != 0x100 || read_u32(xwh, at + 12) != 0 ||
        text_size > xwh.size() - at - 16 || text_size != 32)
        throw std::runtime_error("Unsupported title FTXT version/extent");
    const auto text = at + 16;
    if (read_u32(xwh, text) != 1 || read_u32(xwh, text + 4) != 8)
        throw std::runtime_error("Unsupported title FTXT entry layout");
    constexpr std::string_view name = "thema_1.aif";
    const auto name_at = text + 8;
    if (xwh.size() - name_at < name.size() + 1 ||
        !std::equal(name.begin(), name.end(), xwh.begin() + name_at) ||
        xwh[name_at + name.size()] != 0 ||
        std::any_of(xwh.begin() + name_at + name.size() + 1,
                    xwh.begin() + text + text_size,
                    [](std::uint8_t byte) { return byte != 0; }) ||
        at + 16 + text_size != xwh.size())
        throw std::runtime_error("Unsupported title FTXT name/extent");
    return parsed;
}

std::int64_t arithmetic_shift(std::int64_t value, unsigned bits) {
    const auto divisor = std::int64_t(1) << bits;
    if (value >= 0) return value / divisor;
    return -((-value + divisor - 1) / divisor);
}

std::int16_t read_s16(Bytes bytes, std::size_t offset) {
    const auto raw = read_u16(bytes, offset);
    return std::bit_cast<std::int16_t>(raw);
}

std::int16_t saturate_s16(std::int64_t value) {
    return static_cast<std::int16_t>(std::clamp<std::int64_t>(value, -32768, 32767));
}
} // namespace

TitlePcmStream::TitlePcmStream(
    Bytes xwh, Bytes xwb,
    std::span<const std::int16_t, gain_table_size> q12_gain_table) {
    const auto record = parse_xwh(xwh);
    if (record.body_end > xwb.size())
        throw std::runtime_error("Title XWB body bound exceeds resource");
    const auto body_bytes = std::size_t(record.body_end - record.body_base);
    if (body_bytes == 0 || body_bytes % 4 != 0)
        throw std::runtime_error("Title XWB stereo body extent");

    samples_.assign(xwb.begin() + record.body_base,
                    xwb.begin() + record.body_end);
    std::copy(q12_gain_table.begin(), q12_gain_table.end(), q12_gain_table_.begin());
    if (std::any_of(q12_gain_table_.begin(), q12_gain_table_.end(),
                    [](std::int16_t gain) { return gain < -4096 || gain > 4096; }))
        throw std::runtime_error("Title Q12 gain table range");

    frame_count_ = body_bytes / 4;
    loop_start_frame_ = (record.loop - record.body_base) / 4;
    if (loop_start_frame_ >= frame_count_)
        throw std::runtime_error("Title loop frame outside body");
    authored_left_gain_ = record.left_gain;
    authored_right_gain_ = record.right_gain;
}

int TitlePcmStream::q12_gain(int authored, int stream, int global) const {
    if (stream < -128 || stream > 128 || global < -128 || global > 128)
        throw std::runtime_error("Title runtime gain outside driver range");
    const auto first = arithmetic_shift(std::int64_t(authored) * stream, 7);
    const auto final = arithmetic_shift(first * global, 7);
    const auto clamped = std::clamp<std::int64_t>(final, -128, 128);
    return q12_gain_table_[static_cast<std::size_t>(clamped + 128)];
}

void TitlePcmStream::mix(std::span<std::int16_t> stereo, TitlePcmGains gains) {
    if (stereo.size() % 2 != 0) throw std::runtime_error("Title PCM stereo extent");
    const auto left_gain = q12_gain(authored_left_gain_, gains.stream_left, gains.global_left);
    const auto right_gain = q12_gain(authored_right_gain_, gains.stream_right, gains.global_right);

    const auto frames = stereo.size() / 2;
    for (std::size_t i = 0; i < frames; ++i) {
        if (cursor_frame_ == frame_count_) cursor_frame_ = loop_start_frame_;
        const auto source_at = static_cast<std::size_t>(cursor_frame_) * 4;
        const auto left = read_s16(samples_, source_at);
        const auto right = read_s16(samples_, source_at + 2);
        stereo[i * 2] = saturate_s16(
            std::int64_t(stereo[i * 2]) + arithmetic_shift(std::int64_t(left) * left_gain, 12));
        stereo[i * 2 + 1] = saturate_s16(
            std::int64_t(stereo[i * 2 + 1]) + arithmetic_shift(std::int64_t(right) * right_gain, 12));
        ++cursor_frame_;
    }
}

} // namespace okage
