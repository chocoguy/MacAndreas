#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

struct plm_t;

namespace ma::host::audio {
class Stream;
}

namespace ma::host::media {

class Movie {
public:
    static std::unique_ptr<Movie> open(const std::filesystem::path& path);
    ~Movie();

    void play();
    void pause();
    bool advance();
    bool ended() const { return ended_; }
    bool has_frame() const { return frames_ != 0; }
    const uint8_t* pixels() const { return pixels_.data(); }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    double duration() const { return duration_; }
    uint64_t frames() const { return frames_; }
    void store_frame(void* frame);
    void store_samples(void* samples);
    bool has_audio() const { return audio_ != nullptr; }

private:
    using Clock = std::chrono::steady_clock;

    plm_t* plm_ = nullptr;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    double duration_ = 0;
    std::vector<uint8_t> pixels_;
    uint64_t frames_ = 0;
    bool fresh_ = false;
    bool playing_ = false;
    bool ended_ = false;
    Clock::time_point last_;
    std::shared_ptr<audio::Stream> audio_;
};

}
