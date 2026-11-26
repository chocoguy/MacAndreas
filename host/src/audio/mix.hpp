#pragma once

#include <cstdint>

namespace ma::host::audio {

struct Vec3 {
    float x = 0;
    float y = 0;
    float z = 0;
};

struct Gains {
    float left = 1;
    float right = 1;
};

struct PcmFormat {
    uint16_t channels = 2;
    uint16_t bits = 16;
    uint16_t block_align = 4;
    uint32_t rate = 44100;
};

struct Listener {
    Vec3 position;
    Vec3 velocity;
    Vec3 front{0, 0, 1};
    Vec3 top{0, 1, 0};
    float distance_factor = 1;
    float rolloff = 1;
    float doppler = 1;
};

constexpr uint32_t kMode3dNormal = 0;
constexpr uint32_t kMode3dHeadRelative = 1;
constexpr uint32_t kMode3dDisable = 2;

struct Spatial {
    Vec3 position;
    Vec3 velocity;
    float min_distance = 1;
    float max_distance = 1e9f;
    uint32_t mode = kMode3dNormal;
};

float db_gain(int32_t hundredths);
Gains pan_gains(int32_t pan);
Gains spatial_gains(const Listener& listener, const Spatial& source);

struct Voice {
    const uint8_t* data = nullptr;
    uint32_t bytes = 0;
    PcmFormat format;
    uint32_t frequency = 0;
    Gains gains;
    bool looping = false;
};

bool mix_voice(float* out, uint32_t frames, uint32_t out_rate, const Voice& voice, double& position);

}
