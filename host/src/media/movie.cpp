#include "movie.hpp"

#include <cstddef>
#include <cstdio>

#include "pl_mpeg.h"

#include <algorithm>
#include <cstdio>

namespace ma::host::media {

namespace {

void on_video(plm_t*, plm_frame_t* frame, void* user) { static_cast<Movie*>(user)->store_frame(frame); }

}

std::unique_ptr<Movie> Movie::open(const std::filesystem::path& path) {
    plm_t* plm = plm_create_with_filename(path.c_str());
    if (!plm) {
        return nullptr;
    }
    if (!plm_probe(plm, 5000 * 1024) || plm_get_num_video_streams(plm) == 0) {
        plm_destroy(plm);
        return nullptr;
    }
    auto movie = std::unique_ptr<Movie>(new Movie);
    movie->plm_ = plm;
    movie->width_ = static_cast<uint32_t>(plm_get_width(plm));
    movie->height_ = static_cast<uint32_t>(plm_get_height(plm));
    movie->duration_ = plm_get_duration(plm);
    movie->pixels_.assign(static_cast<size_t>(movie->width_) * movie->height_ * 4, 0);
    plm_set_audio_enabled(plm, 0);
    plm_set_loop(plm, 0);
    plm_set_video_decode_callback(plm, on_video, movie.get());
    return movie;
}

Movie::~Movie() {
    if (plm_) {
        plm_destroy(plm_);
    }
}

void Movie::store_frame(void* frame) {
    plm_frame_to_bgra(static_cast<plm_frame_t*>(frame), pixels_.data(), static_cast<int>(width_ * 4));
    for (size_t i = 3; i < pixels_.size(); i += 4) {
        pixels_[i] = 255;
    }
    ++frames_;
    fresh_ = true;
}

void Movie::play() {
    if (!playing_) {
        playing_ = true;
        last_ = Clock::now();
    }
}

void Movie::pause() { playing_ = false; }

bool Movie::advance() {
    if (!playing_ || ended_) {
        return false;
    }
    auto now = Clock::now();
    double elapsed = std::chrono::duration<double>(now - last_).count();
    last_ = now;
    fresh_ = false;
    plm_decode(plm_, std::min(elapsed, 0.25));
    if (plm_has_ended(plm_)) {
        ended_ = true;
    }
    return fresh_;
}

}
