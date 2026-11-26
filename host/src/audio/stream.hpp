#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace ma::host::audio {

class Stream {
public:
    explicit Stream(uint32_t rate) : rate_(rate) {}

    void push(const float* interleaved, uint32_t frames);
    void clear();
    void mix(float* out, uint32_t frames, uint32_t out_rate);
    size_t queued() const;
    uint32_t rate() const { return rate_; }

private:
    uint32_t rate_;
    mutable std::mutex mutex_;
    std::vector<float> data_;
    size_t read_ = 0;
    double frac_ = 0;
};

void add_stream(const std::shared_ptr<Stream>& stream);
void remove_stream(const Stream* stream);
void mix_streams(float* out, uint32_t frames, uint32_t out_rate);

}
