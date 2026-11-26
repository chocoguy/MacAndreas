#include "com_object.hpp"
#include "process.hpp"

#include "../audio/mix.hpp"
#include "../audio/output.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <vector>
#include <string>

namespace ma::host::win32 {

namespace {

constexpr uint32_t kSpeakerStereo = 4;
constexpr uint32_t kDsErrInvalidParam = 0x80070057u;
constexpr uint32_t kDsErrInvalidCall = 0x88780032u;
constexpr uint32_t kDsErrControlUnavail = 0x8878001eu;
constexpr uint32_t kDsbPrimary = 0x1;
constexpr uint32_t kDsbCtrl3d = 0x10;
constexpr uint32_t kDsbStatusPlaying = 0x1;
constexpr uint32_t kDsbStatusLooping = 0x4;
constexpr uint32_t kDsbPlayLooping = 0x1;

std::recursive_mutex g_mutex;

struct WaveFormat {
    uint16_t tag = 1;
    uint16_t channels = 2;
    uint32_t rate = 44100;
    uint32_t avg_bytes = 176400;
    uint16_t block_align = 4;
    uint16_t bits = 16;
};

struct Buffer;

std::vector<Buffer*>& live_buffers() {
    static std::vector<Buffer*> v;
    return v;
}

audio::Listener g_listener_active;
audio::Listener g_listener_pending;
int32_t g_primary_volume = 0;

struct Buffer {
    uint32_t refs = 1;
    bool primary = false;
    uint32_t flags = 0;
    uint32_t size = 0;
    uint32_t storage = 0;
    WaveFormat format;
    uint32_t frequency = 0;
    int32_t volume = 0;
    int32_t pan = 0;
    bool playing = false;
    bool looping = false;
    double position = 0;
    uint32_t face_buffer = 0;
    uint32_t face_3d = 0;
    uint32_t face_listener = 0;
    audio::Spatial spatial;
    audio::Spatial spatial_pending;

    Buffer() { live_buffers().push_back(this); }

    ~Buffer() {
        std::erase(live_buffers(), this);
        allocator().free(storage);
    }

    uint32_t align() const { return std::max<uint16_t>(format.block_align, 1); }

    uint32_t frames() const { return size / align(); }

    uint32_t bytes_per_second() const {
        uint32_t rate = frequency ? frequency : format.rate;
        return std::max<uint32_t>(1, rate * align());
    }

    uint32_t play_cursor() const {
        uint32_t total = frames();
        if (total == 0) {
            return 0;
        }
        return static_cast<uint32_t>(static_cast<uint64_t>(position) % total) * align();
    }

    uint32_t write_cursor() const {
        if (size == 0) {
            return 0;
        }
        uint32_t lead = std::min<uint32_t>(size, bytes_per_second() / 100) / align() * align();
        return (play_cursor() + lead) % size;
    }

    bool spatialized() const { return (flags & kDsbCtrl3d) != 0 && format.channels == 1; }
};

struct Face : ComObject {
    std::shared_ptr<Buffer> buffer;
};

struct Device : ComObject {};

Buffer* buf(uint32_t self) {
    auto* f = com_get<Face>(self);
    return f ? f->buffer.get() : nullptr;
}

uint32_t buf_add_ref(uint32_t self) {
    std::lock_guard lock(g_mutex);
    Buffer* b = buf(self);
    return b ? ++b->refs : 0;
}

uint32_t buf_release(uint32_t self) {
    std::lock_guard lock(g_mutex);
    Buffer* b = buf(self);
    if (!b || b->refs == 0) {
        return 0;
    }
    if (--b->refs != 0) {
        return b->refs;
    }
    for (uint32_t face : {b->face_buffer, b->face_3d, b->face_listener}) {
        if (face != 0) {
            com_objects().destroy(face);
        }
    }
    return 0;
}

uint32_t buf_query_interface(uint32_t self, uint32_t riid, uint32_t out);

uint32_t buf_get_caps(uint32_t self, uint32_t caps) {
    std::lock_guard lock(g_mutex);
    Buffer* b = buf(self);
    if (!b || caps == 0) {
        return kDsErrInvalidParam;
    }
    mem::write<uint32_t>(caps + 4, b->flags);
    mem::write<uint32_t>(caps + 8, b->size);
    mem::write<uint32_t>(caps + 12, 0);
    mem::write<uint32_t>(caps + 16, 0);
    return kSOk;
}

uint32_t buf_get_current_position(uint32_t self, uint32_t play, uint32_t write) {
    std::lock_guard lock(g_mutex);
    Buffer* b = buf(self);
    if (!b) {
        return kDsErrInvalidParam;
    }
    com_out(play, b->play_cursor());
    com_out(write, b->write_cursor());
    return kSOk;
}

void write_format(uint32_t out, const WaveFormat& f) {
    mem::write<uint16_t>(out, f.tag);
    mem::write<uint16_t>(out + 2, f.channels);
    mem::write<uint32_t>(out + 4, f.rate);
    mem::write<uint32_t>(out + 8, f.avg_bytes);
    mem::write<uint16_t>(out + 12, f.block_align);
    mem::write<uint16_t>(out + 14, f.bits);
}

WaveFormat read_format(uint32_t in) {
    WaveFormat f;
    f.tag = mem::read<uint16_t>(in);
    f.channels = mem::read<uint16_t>(in + 2);
    f.rate = mem::read<uint32_t>(in + 4);
    f.avg_bytes = mem::read<uint32_t>(in + 8);
    f.block_align = mem::read<uint16_t>(in + 12);
    f.bits = mem::read<uint16_t>(in + 14);
    return f;
}

uint32_t buf_get_format(uint32_t self, uint32_t out, uint32_t size, uint32_t written) {
    std::lock_guard lock(g_mutex);
    Buffer* b = buf(self);
    if (!b) {
        return kDsErrInvalidParam;
    }
    if (out != 0 && size >= 16) {
        write_format(out, b->format);
        if (size >= 18) {
            mem::write<uint16_t>(out + 16, 0);
        }
    }
    com_out(written, 18);
    return kSOk;
}

uint32_t buf_get_volume(uint32_t self, uint32_t out) {
    std::lock_guard lock(g_mutex);
    Buffer* b = buf(self);
    com_out(out, b ? static_cast<uint32_t>(b->volume) : 0);
    return kSOk;
}

uint32_t buf_get_pan(uint32_t self, uint32_t out) {
    std::lock_guard lock(g_mutex);
    Buffer* b = buf(self);
    com_out(out, b ? static_cast<uint32_t>(b->pan) : 0);
    return kSOk;
}

uint32_t buf_get_frequency(uint32_t self, uint32_t out) {
    std::lock_guard lock(g_mutex);
    Buffer* b = buf(self);
    com_out(out, b ? (b->frequency ? b->frequency : b->format.rate) : 0);
    return kSOk;
}

uint32_t buf_get_status(uint32_t self, uint32_t out) {
    std::lock_guard lock(g_mutex);
    Buffer* b = buf(self);
    uint32_t status = 0;
    if (b) {
        if (b->playing) {
            status = kDsbStatusPlaying | (b->looping ? kDsbStatusLooping : 0);
        }
    }
    com_out(out, status);
    return kSOk;
}

uint32_t buf_lock(uint32_t self, uint32_t offset, uint32_t bytes, uint32_t p1, uint32_t b1, uint32_t p2, uint32_t b2,
                  uint32_t flags) {
    constexpr uint32_t kFromWriteCursor = 0x1;
    constexpr uint32_t kEntireBuffer = 0x2;
    std::lock_guard lock(g_mutex);
    Buffer* b = buf(self);
    if (!b || b->size == 0 || p1 == 0 || b1 == 0) {
        return kDsErrInvalidParam;
    }
    if (flags & kFromWriteCursor) {
        offset = b->write_cursor();
    }
    if (flags & kEntireBuffer) {
        bytes = b->size;
    }
    if (offset >= b->size || bytes > b->size) {
        return kDsErrInvalidParam;
    }
    uint32_t first = std::min(bytes, b->size - offset);
    mem::write<uint32_t>(p1, b->storage + offset);
    mem::write<uint32_t>(b1, first);
    com_out(p2, first < bytes ? b->storage : 0);
    com_out(b2, bytes - first);
    return kSOk;
}

uint32_t buf_play(uint32_t self, uint32_t reserved, uint32_t priority, uint32_t flags) {
    std::lock_guard lock(g_mutex);
    Buffer* b = buf(self);
    if (!b) {
        return kDsErrInvalidParam;
    }
    b->looping = (flags & kDsbPlayLooping) != 0 || b->primary;
    b->playing = true;
    return kSOk;
}

uint32_t buf_set_current_position(uint32_t self, uint32_t position) {
    std::lock_guard lock(g_mutex);
    Buffer* b = buf(self);
    if (!b) {
        return kDsErrInvalidParam;
    }
    b->position = b->size ? static_cast<double>((position % b->size) / b->align()) : 0;
    return kSOk;
}

uint32_t buf_set_format(uint32_t self, uint32_t wfx) {
    std::lock_guard lock(g_mutex);
    Buffer* b = buf(self);
    if (!b || wfx == 0) {
        return kDsErrInvalidParam;
    }
    b->format = read_format(wfx);
    return kSOk;
}

uint32_t buf_set_volume(uint32_t self, int32_t v) {
    std::lock_guard lock(g_mutex);
    if (Buffer* b = buf(self)) {
        b->volume = std::clamp(v, -10000, 0);
        if (b->primary) {
            g_primary_volume = b->volume;
        }
    }
    return kSOk;
}

uint32_t buf_set_pan(uint32_t self, int32_t v) {
    std::lock_guard lock(g_mutex);
    if (Buffer* b = buf(self)) {
        b->pan = std::clamp(v, -10000, 10000);
    }
    return kSOk;
}

uint32_t buf_set_frequency(uint32_t self, uint32_t v) {
    std::lock_guard lock(g_mutex);
    if (Buffer* b = buf(self)) {
        b->frequency = v;
    }
    return kSOk;
}

uint32_t buf_stop(uint32_t self) {
    std::lock_guard lock(g_mutex);
    if (Buffer* b = buf(self)) {
        b->playing = false;
    }
    return kSOk;
}

uint32_t buf_unlock(uint32_t self, uint32_t p1, uint32_t b1, uint32_t p2, uint32_t b2) { return kSOk; }
uint32_t buf_restore(uint32_t self) { return kSOk; }

audio::Vec3 vec(float x, float y, float z) { return {x, y, z}; }

void write_vec(uint32_t out, const audio::Vec3& v) {
    if (out != 0) {
        mem::write<float>(out, v.x);
        mem::write<float>(out + 4, v.y);
        mem::write<float>(out + 8, v.z);
    }
}

void write_float(uint32_t out, float v) {
    if (out != 0) {
        mem::write<float>(out, v);
    }
}

constexpr uint32_t kDs3dImmediate = 0;

template <typename F>
uint32_t set_spatial(uint32_t self, uint32_t apply, F&& fn) {
    std::lock_guard lock(g_mutex);
    Buffer* b = buf(self);
    if (!b) {
        return kDsErrInvalidParam;
    }
    fn(b->spatial_pending);
    if (apply == kDs3dImmediate) {
        fn(b->spatial);
    }
    return kSOk;
}

template <typename F>
uint32_t get_spatial(uint32_t self, F&& fn) {
    std::lock_guard lock(g_mutex);
    Buffer* b = buf(self);
    if (!b) {
        return kDsErrInvalidParam;
    }
    fn(b->spatial);
    return kSOk;
}

uint32_t b3d_set_position(uint32_t self, float x, float y, float z, uint32_t apply) {
    return set_spatial(self, apply, [&](audio::Spatial& s) { s.position = vec(x, y, z); });
}

uint32_t b3d_set_velocity(uint32_t self, float x, float y, float z, uint32_t apply) {
    return set_spatial(self, apply, [&](audio::Spatial& s) { s.velocity = vec(x, y, z); });
}

uint32_t b3d_set_min_distance(uint32_t self, float d, uint32_t apply) {
    return set_spatial(self, apply, [&](audio::Spatial& s) { s.min_distance = d; });
}

uint32_t b3d_set_max_distance(uint32_t self, float d, uint32_t apply) {
    return set_spatial(self, apply, [&](audio::Spatial& s) { s.max_distance = d; });
}

uint32_t b3d_set_mode(uint32_t self, uint32_t mode, uint32_t apply) {
    return set_spatial(self, apply, [&](audio::Spatial& s) { s.mode = mode; });
}

uint32_t b3d_get_mode(uint32_t self, uint32_t out) {
    return get_spatial(self, [&](const audio::Spatial& s) { com_out(out, s.mode); });
}

uint32_t b3d_get_position(uint32_t self, uint32_t out) {
    return get_spatial(self, [&](const audio::Spatial& s) { write_vec(out, s.position); });
}

uint32_t b3d_get_velocity(uint32_t self, uint32_t out) {
    return get_spatial(self, [&](const audio::Spatial& s) { write_vec(out, s.velocity); });
}

uint32_t b3d_get_min_distance(uint32_t self, uint32_t out) {
    return get_spatial(self, [&](const audio::Spatial& s) { write_float(out, s.min_distance); });
}

uint32_t b3d_get_max_distance(uint32_t self, uint32_t out) {
    return get_spatial(self, [&](const audio::Spatial& s) { write_float(out, s.max_distance); });
}

template <typename F>
uint32_t set_listener(uint32_t apply, F&& fn) {
    std::lock_guard lock(g_mutex);
    fn(g_listener_pending);
    if (apply == kDs3dImmediate) {
        fn(g_listener_active);
    }
    return kSOk;
}

uint32_t lis_set_position(uint32_t self, float x, float y, float z, uint32_t apply) {
    return set_listener(apply, [&](audio::Listener& l) { l.position = vec(x, y, z); });
}

uint32_t lis_set_velocity(uint32_t self, float x, float y, float z, uint32_t apply) {
    return set_listener(apply, [&](audio::Listener& l) { l.velocity = vec(x, y, z); });
}

uint32_t lis_set_orientation(uint32_t self, float fx, float fy, float fz, float tx, float ty, float tz, uint32_t apply) {
    return set_listener(apply, [&](audio::Listener& l) {
        l.front = vec(fx, fy, fz);
        l.top = vec(tx, ty, tz);
    });
}

uint32_t lis_set_distance_factor(uint32_t self, float v, uint32_t apply) {
    return set_listener(apply, [&](audio::Listener& l) { l.distance_factor = v; });
}

uint32_t lis_set_doppler_factor(uint32_t self, float v, uint32_t apply) {
    return set_listener(apply, [&](audio::Listener& l) { l.doppler = v; });
}

uint32_t lis_set_rolloff_factor(uint32_t self, float v, uint32_t apply) {
    return set_listener(apply, [&](audio::Listener& l) { l.rolloff = v; });
}

uint32_t lis_get_position(uint32_t self, uint32_t out) {
    std::lock_guard lock(g_mutex);
    write_vec(out, g_listener_active.position);
    return kSOk;
}

uint32_t lis_get_velocity(uint32_t self, uint32_t out) {
    std::lock_guard lock(g_mutex);
    write_vec(out, g_listener_active.velocity);
    return kSOk;
}

uint32_t lis_get_orientation(uint32_t self, uint32_t front, uint32_t top) {
    std::lock_guard lock(g_mutex);
    write_vec(front, g_listener_active.front);
    write_vec(top, g_listener_active.top);
    return kSOk;
}

uint32_t lis_get_distance_factor(uint32_t self, uint32_t out) {
    std::lock_guard lock(g_mutex);
    write_float(out, g_listener_active.distance_factor);
    return kSOk;
}

uint32_t lis_get_doppler_factor(uint32_t self, uint32_t out) {
    std::lock_guard lock(g_mutex);
    write_float(out, g_listener_active.doppler);
    return kSOk;
}

uint32_t lis_get_rolloff_factor(uint32_t self, uint32_t out) {
    std::lock_guard lock(g_mutex);
    write_float(out, g_listener_active.rolloff);
    return kSOk;
}

uint32_t lis_commit(uint32_t self) {
    std::lock_guard lock(g_mutex);
    g_listener_active = g_listener_pending;
    for (Buffer* b : live_buffers()) {
        b->spatial = b->spatial_pending;
    }
    return kSOk;
}

void render(float* out, uint32_t frames, uint32_t rate) {
    std::lock_guard lock(g_mutex);
    float master = audio::db_gain(g_primary_volume);
    for (Buffer* b : live_buffers()) {
        if (b->primary || !b->playing || b->size == 0 || b->storage == 0) {
            continue;
        }
        audio::Voice v;
        v.data = mem::ptr(b->storage);
        v.bytes = b->size;
        v.format = {b->format.channels, b->format.bits, b->format.block_align, b->format.rate};
        v.frequency = b->frequency;
        v.looping = b->looping;
        float gain = audio::db_gain(b->volume) * master;
        audio::Gains g = b->spatialized() ? audio::spatial_gains(g_listener_active, b->spatial) : audio::pan_gains(b->pan);
        v.gains = {g.left * gain, g.right * gain};
        if (!audio::mix_voice(out, frames, rate, v, b->position)) {
            b->playing = false;
            b->position = 0;
        }
    }
    for (uint32_t i = 0; i < 2 * frames; ++i) {
        out[i] = std::clamp(out[i], -1.0f, 1.0f);
    }
}

#define DS_UNKNOWN(QI)                                                                                \
    com_method<QI>("QueryInterface"), com_method<buf_add_ref>("AddRef"), com_method<buf_release>("Release")

ComInterface g_buffer("dsound.dll", "IDirectSoundBuffer8",
                      {
                          DS_UNKNOWN(buf_query_interface),
                          com_method<buf_get_caps>("GetCaps"),
                          com_method<buf_get_current_position>("GetCurrentPosition"),
                          com_method<buf_get_format>("GetFormat"),
                          com_method<buf_get_volume>("GetVolume"),
                          com_method<buf_get_pan>("GetPan"),
                          com_method<buf_get_frequency>("GetFrequency"),
                          com_method<buf_get_status>("GetStatus"),
                          ComMethod{"Initialize", 3, nullptr, kDsErrInvalidCall},
                          com_method<buf_lock>("Lock"),
                          com_method<buf_play>("Play"),
                          com_method<buf_set_current_position>("SetCurrentPosition"),
                          com_method<buf_set_format>("SetFormat"),
                          com_method<buf_set_volume>("SetVolume"),
                          com_method<buf_set_pan>("SetPan"),
                          com_method<buf_set_frequency>("SetFrequency"),
                          com_method<buf_stop>("Stop"),
                          com_method<buf_unlock>("Unlock"),
                          com_method<buf_restore>("Restore"),
                          ComMethod{"SetFX", 4, nullptr, kDsErrControlUnavail},
                          ComMethod{"AcquireResources", 4, com_noop},
                          ComMethod{"GetObjectInPath", 5, nullptr, kENoInterface},
                      });

ComInterface g_buffer3d("dsound.dll", "IDirectSound3DBuffer",
                        {
                            DS_UNKNOWN(buf_query_interface),
                            ComMethod{"GetAllParameters", 2, nullptr, kENotImpl},
                            ComMethod{"GetConeAngles", 3, com_noop},
                            ComMethod{"GetConeOrientation", 2, com_noop},
                            ComMethod{"GetConeOutsideVolume", 2, com_noop},
                            com_method<b3d_get_max_distance>("GetMaxDistance"),
                            com_method<b3d_get_min_distance>("GetMinDistance"),
                            com_method<b3d_get_mode>("GetMode"),
                            com_method<b3d_get_position>("GetPosition"),
                            com_method<b3d_get_velocity>("GetVelocity"),
                            ComMethod{"SetAllParameters", 3, nullptr, kENotImpl},
                            ComMethod{"SetConeAngles", 4, com_noop},
                            ComMethod{"SetConeOrientation", 5, com_noop},
                            ComMethod{"SetConeOutsideVolume", 3, com_noop},
                            com_method<b3d_set_max_distance>("SetMaxDistance"),
                            com_method<b3d_set_min_distance>("SetMinDistance"),
                            com_method<b3d_set_mode>("SetMode"),
                            com_method<b3d_set_position>("SetPosition"),
                            com_method<b3d_set_velocity>("SetVelocity"),
                        });

ComInterface g_listener("dsound.dll", "IDirectSound3DListener",
                        {
                            DS_UNKNOWN(buf_query_interface),
                            ComMethod{"GetAllParameters", 2, nullptr, kENotImpl},
                            com_method<lis_get_distance_factor>("GetDistanceFactor"),
                            com_method<lis_get_doppler_factor>("GetDopplerFactor"),
                            com_method<lis_get_orientation>("GetOrientation"),
                            com_method<lis_get_position>("GetPosition"),
                            com_method<lis_get_rolloff_factor>("GetRolloffFactor"),
                            com_method<lis_get_velocity>("GetVelocity"),
                            ComMethod{"SetAllParameters", 3, nullptr, kENotImpl},
                            com_method<lis_set_distance_factor>("SetDistanceFactor"),
                            com_method<lis_set_doppler_factor>("SetDopplerFactor"),
                            com_method<lis_set_orientation>("SetOrientation"),
                            com_method<lis_set_position>("SetPosition"),
                            com_method<lis_set_rolloff_factor>("SetRolloffFactor"),
                            com_method<lis_set_velocity>("SetVelocity"),
                            com_method<lis_commit>("CommitDeferredSettings"),
                        });

bool ds_iid(uint32_t riid, uint32_t d1) {
    return guid_equals(riid, d1, 0x4981, 0x11ce, {0xa5, 0x21, 0x00, 0x20, 0xaf, 0x0b, 0xe5, 0x60});
}

uint32_t make_face(ComInterface& iface, const std::shared_ptr<Buffer>& b) {
    auto f = std::make_unique<Face>();
    f->buffer = b;
    return com_objects().wrap(iface, std::move(f));
}

uint32_t buf_query_interface(uint32_t self, uint32_t riid, uint32_t out) {
    std::lock_guard lock(g_mutex);
    auto* f = com_get<Face>(self);
    com_out(out, 0);
    if (!f) {
        return kENoInterface;
    }
    Buffer& b = *f->buffer;
    uint32_t face = 0;
    if (ds_iid(riid, 0x279afa85) || guid_equals(riid, 0x6825a449, 0x7524, 0x4d82,
                                                 {0x92, 0x0f, 0x50, 0xe3, 0x6a, 0xb3, 0xab, 0x1e})) {
        face = b.face_buffer;
    } else if (ds_iid(riid, 0x279afa86) && (b.flags & kDsbCtrl3d) && !b.primary) {
        if (!b.face_3d) {
            b.face_3d = make_face(g_buffer3d, f->buffer);
        }
        face = b.face_3d;
    } else if (ds_iid(riid, 0x279afa84) && b.primary) {
        if (!b.face_listener) {
            b.face_listener = make_face(g_listener, f->buffer);
        }
        face = b.face_listener;
    }
    if (face == 0) {
        static std::set<std::string> seen;
        if (seen.insert(guid_string(riid)).second) {
            std::fprintf(stderr, "[DSOUND] QueryInterface(%s) refused\n", guid_string(riid).c_str());
        }
        return kENoInterface;
    }
    ++b.refs;
    com_out(out, face);
    return kSOk;
}

uint32_t create_buffer(const std::shared_ptr<Buffer>& b) {
    b->face_buffer = make_face(g_buffer, b);
    return b->face_buffer;
}

uint32_t ds_query_interface(uint32_t self, uint32_t riid, uint32_t out) {
    com_out(out, 0);
    return kENoInterface;
}

uint32_t ds_create_sound_buffer(uint32_t self, uint32_t desc, uint32_t out, uint32_t outer) {
    std::lock_guard lock(g_mutex);
    if (desc == 0 || out == 0) {
        return kDsErrInvalidParam;
    }
    auto b = std::make_shared<Buffer>();
    b->flags = mem::read<uint32_t>(desc + 4);
    b->primary = (b->flags & kDsbPrimary) != 0;
    uint32_t wfx = mem::read<uint32_t>(desc + 16);
    if (wfx != 0) {
        b->format = read_format(wfx);
    }
    b->size = b->primary ? 0x10000 : mem::read<uint32_t>(desc + 8);
    if (b->size == 0 && !b->primary) {
        return kDsErrInvalidParam;
    }
    b->storage = allocator().alloc(b->size);
    com_out(out, create_buffer(b));
    return kSOk;
}

uint32_t ds_get_caps(uint32_t self, uint32_t caps) {
    if (caps == 0) {
        return kDsErrInvalidParam;
    }
    std::memset(mem::ptr(caps + 4), 0, 92);
    mem::write<uint32_t>(caps + 4, 0x1 | 0x2 | 0x4 | 0x8 | 0x10 | 0x100 | 0x200 | 0x400 | 0x800);
    mem::write<uint32_t>(caps + 8, 100);
    mem::write<uint32_t>(caps + 12, 200000);
    mem::write<uint32_t>(caps + 16, 1);
    for (uint32_t off = 20; off <= 64; off += 4) {
        mem::write<uint32_t>(caps + off, 64);
    }
    return kSOk;
}

uint32_t ds_duplicate(uint32_t self, uint32_t original, uint32_t out) {
    std::lock_guard lock(g_mutex);
    Buffer* src = buf(original);
    if (!src || out == 0) {
        return kDsErrInvalidParam;
    }
    auto b = std::make_shared<Buffer>();
    b->flags = src->flags;
    b->format = src->format;
    b->size = src->size;
    b->storage = allocator().alloc(b->size);
    std::memcpy(mem::ptr(b->storage), mem::ptr(src->storage), b->size);
    com_out(out, create_buffer(b));
    return kSOk;
}

uint32_t ds_set_cooperative_level(uint32_t self, uint32_t hwnd, uint32_t level) { return kSOk; }

uint32_t ds_get_speaker_config(uint32_t self, uint32_t out) {
    com_out(out, kSpeakerStereo);
    return kSOk;
}

ComInterface g_device("dsound.dll", "IDirectSound8",
                      {
                          com_method<ds_query_interface>("QueryInterface"),
                          com_method<com_obj_add_ref>("AddRef"),
                          com_method<com_obj_release>("Release"),
                          com_method<ds_create_sound_buffer>("CreateSoundBuffer"),
                          com_method<ds_get_caps>("GetCaps"),
                          com_method<ds_duplicate>("DuplicateSoundBuffer"),
                          com_method<ds_set_cooperative_level>("SetCooperativeLevel"),
                          ComMethod{"Compact", 1, com_noop},
                          com_method<ds_get_speaker_config>("GetSpeakerConfig"),
                          ComMethod{"SetSpeakerConfig", 2, com_noop},
                          ComMethod{"Initialize", 2, com_noop},
                          ComMethod{"VerifyCertification", 2, com_noop},
                      });

uint32_t direct_sound_create8(uint32_t guid, uint32_t out, uint32_t outer) {
    if (out == 0) {
        return kDsErrInvalidParam;
    }
    audio::start_output(render, audio::default_options());
    com_out(out, com_objects().wrap(g_device, std::make_unique<Device>()));
    return kSOk;
}

constexpr ApiDef kDsound[] = {
    native_export<Conv::stdcall, direct_sound_create8>("EAX.DLL", "", 6),
    native_export<Conv::stdcall, direct_sound_create8>("dsound.dll", "", 11),
    native_export<Conv::stdcall, direct_sound_create8>("dsound.dll", "DirectSoundCreate8"),
};

}

std::span<const ApiDef> dsound_apis() { return kDsound; }

void dsound_render(float* out, uint32_t frames, uint32_t rate) { render(out, frames, rate); }

}
