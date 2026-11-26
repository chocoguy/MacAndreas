#include "perf.hpp"

#include "settings.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <mutex>
#include <vector>

namespace ma::host::perf {

namespace {

struct Recorder {
    std::mutex mutex;
    bool started = false;
    bool finished = false;
    std::chrono::steady_clock::time_point last;
    std::vector<float> frames;
};

Recorder& recorder() {
    static Recorder* r = new Recorder;
    return *r;
}

double percentile(const std::vector<float>& sorted, double p) {
    if (sorted.empty()) {
        return 0;
    }
    auto index = static_cast<size_t>(std::ceil(p * static_cast<double>(sorted.size()))) - 1;
    return sorted[std::min(index, sorted.size() - 1)];
}

}

FrameStats summarize(std::span<const float> frame_ms) {
    FrameStats s;
    s.frames = frame_ms.size();
    if (frame_ms.empty()) {
        return s;
    }
    std::vector<float> sorted(frame_ms.begin(), frame_ms.end());
    std::ranges::sort(sorted);
    double total = 0;
    for (float ms : sorted) {
        total += ms;
        s.over_20ms += ms > 20.0f ? 1 : 0;
        s.over_33ms += ms > 33.4f ? 1 : 0;
    }
    s.mean_ms = total / static_cast<double>(sorted.size());
    s.p50_ms = percentile(sorted, 0.50);
    s.p95_ms = percentile(sorted, 0.95);
    s.p99_ms = percentile(sorted, 0.99);
    s.max_ms = sorted.back();
    return s;
}

std::string format_stats(const FrameStats& s) {
    return std::format("{} frames, mean {:.2f} ms ({:.1f} fps), p50 {:.2f}, p95 {:.2f}, p99 {:.2f}, max {:.1f} ms, "
                       "{} over 20 ms, {} over 33 ms",
                       s.frames, s.mean_ms, s.mean_ms > 0 ? 1000.0 / s.mean_ms : 0.0, s.p50_ms, s.p95_ms, s.p99_ms,
                       s.max_ms, s.over_20ms, s.over_33ms);
}

void frame_presented() {
    if (settings().frame_times.empty()) {
        return;
    }
    Recorder& r = recorder();
    auto now = std::chrono::steady_clock::now();
    std::lock_guard lock(r.mutex);
    if (!r.started) {
        r.started = true;
        r.last = now;
        std::atexit(finish);
        return;
    }
    r.frames.push_back(std::chrono::duration<float, std::milli>(now - r.last).count());
    r.last = now;
}

void finish() {
    Recorder& r = recorder();
    std::lock_guard lock(r.mutex);
    if (!r.started || r.finished) {
        return;
    }
    r.finished = true;
    const auto& path = settings().frame_times;
    if (FILE* f = std::fopen(path.c_str(), "w")) {
        std::fprintf(f, "frame,ms\n");
        for (size_t i = 0; i < r.frames.size(); ++i) {
            std::fprintf(f, "%zu,%.3f\n", i + 2, r.frames[i]);
        }
        std::fclose(f);
    } else {
        std::fprintf(stderr, "[PERF] cannot write %s\n", path.c_str());
    }
    std::fprintf(stderr, "[PERF] %s\n", format_stats(summarize(r.frames)).c_str());
}

}
