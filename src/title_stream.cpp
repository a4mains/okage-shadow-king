#include "okage/title_stream.hpp"

#include <stdexcept>

namespace okage {

void TitleStreamOwner::release_locked() {
    stop_pending_ = false;
    stop_countdown_ = 0;
    service_ready_ = false;
    gain_update_pending_ = false;
    requested_volume_ = 0;
    stream_volume_ = 0;
    stream_.reset();
}

void TitleStreamOwner::start(
    unsigned slot, Bytes xwh, Bytes xwb,
    std::span<const std::int16_t, TitlePcmStream::gain_table_size> q12_gain_table) {
    if (slot != title_slot)
        throw std::runtime_error("Title stream owner supports source slot 1 only");

    std::lock_guard lock(mutex_);
    if (stream_ && !stop_pending_)
        throw std::runtime_error("Title stream slot 1 is already active");

    if (stop_pending_) release_locked();

    auto next = std::make_unique<TitlePcmStream>(xwh, xwb, q12_gain_table);
    stream_ = std::move(next);
    stop_pending_ = false;
    stop_countdown_ = 0;
    service_ready_ = false;
    gain_update_pending_ = true;
    requested_volume_ = 128; // 12dea0 queues 1757e0(...,128,128).
    stream_volume_ = 0;
}

void TitleStreamOwner::request_stop(unsigned slot) {
    if (slot != title_slot)
        throw std::runtime_error("Title stream stop supports source slot 1 only");
    std::lock_guard lock(mutex_);
    // Exact 12dd50 stores: pending=1, countdown=128, slot=argument.
    stop_pending_ = true;
    stop_countdown_ = deferred_stop_updates;
}

void TitleStreamOwner::advance_main_loop() {
    std::lock_guard lock(mutex_);
    if (!stream_) return;

    if (gain_update_pending_) {
        stream_volume_ = requested_volume_;
        gain_update_pending_ = false;
    }
    service_ready_ = true;

    if (!stop_pending_) return;

    if (stop_countdown_ != 0) --stop_countdown_;
    requested_volume_ = static_cast<int>(stop_countdown_);
    gain_update_pending_ = true;
    if (stop_countdown_ == 0) {
        // The source clears 2d9a60 before 12de98 and 12dcb0(slot).
        release_locked();
    }
}

void TitleStreamOwner::mix(std::span<std::int16_t> interleaved_stereo) {
    std::lock_guard lock(mutex_);
    if (!stream_ || !service_ready_) return;
    const auto volume = stream_volume_;
    stream_->mix(interleaved_stereo, {volume, volume, 128, 128});
}

TitleStreamState TitleStreamOwner::state() const {
    std::lock_guard lock(mutex_);
    return {bool(stream_), stop_pending_,
            (stream_ || stop_pending_) ? title_slot : 0,
            stop_countdown_, service_ready_, requested_volume_, stream_volume_,
            stream_ ? stream_->cursor_frame() : 0};
}

} // namespace okage
