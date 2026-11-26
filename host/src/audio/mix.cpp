#include "mix.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ma::host::audio {

namespace {

float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

Vec3 sub(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }

Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

float length(const Vec3& v) { return std::sqrt(dot(v, v)); }

Vec3 normalized(const Vec3& v, const Vec3& fallback) {
    float l = length(v);
    if (!(l > 1e-6f)) {
        return fallback;
    }
    return {v.x / l, v.y / l, v.z / l};
}

float sample(const uint8_t* data, uint32_t frame, uint16_t channel, const PcmFormat& f) {
    const uint8_t* p = data + static_cast<size_t>(frame) * f.block_align;
    if (f.bits == 8) {
        return (static_cast<float>(p[channel]) - 128.0f) / 128.0f;
    }
    int16_t v;
    std::memcpy(&v, p + channel * 2, 2);
    return static_cast<float>(v) / 32768.0f;
}

}

float db_gain(int32_t hundredths) {
    if (hundredths <= -10000) {
        return 0;
    }
    if (hundredths >= 0) {
        return 1;
    }
    return std::pow(10.0f, static_cast<float>(hundredths) / 2000.0f);
}

Gains pan_gains(int32_t pan) {
    pan = std::clamp(pan, -10000, 10000);
    if (pan > 0) {
        return {db_gain(-pan), 1};
    }
    return {1, db_gain(pan)};
}

Gains spatial_gains(const Listener& listener, const Spatial& source) {
    if (source.mode == kMode3dDisable) {
        return {};
    }
    Vec3 rel = source.mode == kMode3dHeadRelative ? source.position : sub(source.position, listener.position);
    Vec3 front = normalized(listener.front, {0, 0, 1});
    Vec3 top = normalized(listener.top, {0, 1, 0});
    Vec3 right = normalized(cross(top, front), {1, 0, 0});
    float distance = length(rel) * std::max(listener.distance_factor, 0.0f);
    float gain = 1;
    float min_d = std::max(source.min_distance, 1e-6f);
    if (listener.rolloff > 0 && distance > min_d) {
        float d = std::min(distance, std::max(source.max_distance, min_d));
        gain = min_d / (min_d + listener.rolloff * (d - min_d));
    }
    float side = 0;
    float l = length(rel);
    if (l > 1e-6f) {
        side = std::clamp(dot(rel, right) / l, -1.0f, 1.0f);
    }
    return {gain * std::min(1.0f, std::sqrt(1.0f - side)), gain * std::min(1.0f, std::sqrt(1.0f + side))};
}

bool mix_voice(float* out, uint32_t frames, uint32_t out_rate, const Voice& v, double& position) {
    const PcmFormat& f = v.format;
    if (!v.data || f.block_align == 0 || (f.bits != 8 && f.bits != 16) || f.channels == 0 || out_rate == 0) {
        return false;
    }
    uint32_t total = v.bytes / f.block_align;
    if (total == 0) {
        return false;
    }
    uint32_t rate = v.frequency ? v.frequency : f.rate;
    double step = static_cast<double>(rate) / out_rate;
    bool stereo = f.channels >= 2;
    bool silent = v.gains.left == 0 && v.gains.right == 0;
    for (uint32_t i = 0; i < frames; ++i) {
        if (position >= total) {
            if (!v.looping) {
                return false;
            }
            position = std::fmod(position, static_cast<double>(total));
        }
        if (!silent) {
            auto i0 = static_cast<uint32_t>(position);
            uint32_t i1 = i0 + 1;
            float t = static_cast<float>(position - i0);
            bool has_next = i1 < total || v.looping;
            if (i1 >= total) {
                i1 = 0;
            }
            float l0 = sample(v.data, i0, 0, f);
            float r0 = stereo ? sample(v.data, i0, 1, f) : l0;
            float l = l0;
            float r = r0;
            if (has_next && t > 0) {
                float l1 = sample(v.data, i1, 0, f);
                float r1 = stereo ? sample(v.data, i1, 1, f) : l1;
                l += (l1 - l0) * t;
                r += (r1 - r0) * t;
            }
            out[2 * i] += l * v.gains.left;
            out[2 * i + 1] += r * v.gains.right;
        }
        position += step;
    }
    if (position >= total) {
        if (!v.looping) {
            return false;
        }
        position = std::fmod(position, static_cast<double>(total));
    }
    return true;
}

}
