#include "stream.hpp"

#include <algorithm>

namespace ma::host::audio {

namespace {

struct Registry {
    std::mutex mutex;
    std::vector<std::shared_ptr<Stream>> streams;
};

Registry& registry() {
    static Registry* r = new Registry;
    return *r;
}

}

void Stream::push(const float* interleaved, uint32_t frames) {
    std::lock_guard lock(mutex_);
    data_.insert(data_.end(), interleaved, interleaved + 2 * static_cast<size_t>(frames));
}

void Stream::clear() {
    std::lock_guard lock(mutex_);
    data_.clear();
    read_ = 0;
    frac_ = 0;
}

size_t Stream::queued() const {
    std::lock_guard lock(mutex_);
    return data_.size() / 2 - read_;
}

void Stream::mix(float* out, uint32_t frames, uint32_t out_rate) {
    std::lock_guard lock(mutex_);
    if (out_rate == 0 || rate_ == 0) {
        return;
    }
    double step = static_cast<double>(rate_) / out_rate;
    size_t total = data_.size() / 2;
    for (uint32_t i = 0; i < frames; ++i) {
        if (read_ + 1 >= total) {
            break;
        }
        auto t = static_cast<float>(frac_);
        const float* a = &data_[2 * read_];
        const float* b = a + 2;
        out[2 * i] += a[0] + (b[0] - a[0]) * t;
        out[2 * i + 1] += a[1] + (b[1] - a[1]) * t;
        frac_ += step;
        auto whole = static_cast<size_t>(frac_);
        read_ += whole;
        frac_ -= static_cast<double>(whole);
    }
    if (read_ >= 4096) {
        size_t drop = std::min(read_, total);
        data_.erase(data_.begin(), data_.begin() + static_cast<std::ptrdiff_t>(2 * drop));
        read_ -= drop;
    }
}

void add_stream(const std::shared_ptr<Stream>& stream) {
    std::lock_guard lock(registry().mutex);
    auto& v = registry().streams;
    if (std::ranges::find(v, stream) == v.end()) {
        v.push_back(stream);
    }
}

void remove_stream(const Stream* stream) {
    std::lock_guard lock(registry().mutex);
    std::erase_if(registry().streams, [&](const auto& s) { return s.get() == stream; });
}

void mix_streams(float* out, uint32_t frames, uint32_t out_rate) {
    std::lock_guard lock(registry().mutex);
    for (const auto& s : registry().streams) {
        s->mix(out, frames, out_rate);
    }
}

}
