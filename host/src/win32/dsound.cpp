#include "com_object.hpp"
#include "process.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <string>

namespace ma::host::win32 {

namespace {

constexpr uint32_t kSpeakerStereo = 4;
constexpr uint32_t kDsErrInvalidParam = 0x80070057u;
constexpr uint32_t kDsErrInvalidCall = 0x88780032u;
constexpr uint32_t kDsbPrimary = 0x1;
constexpr uint32_t kDsbCtrl3d = 0x10;
constexpr uint32_t kDsbStatusPlaying = 0x1;
constexpr uint32_t kDsbStatusLooping = 0x4;
constexpr uint32_t kDsbPlayLooping = 0x1;

std::recursive_mutex g_mutex;

using Clock = std::chrono::steady_clock;

struct WaveFormat {
    uint16_t tag = 1;
    uint16_t channels = 2;
    uint32_t rate = 44100;
    uint32_t avg_bytes = 176400;
    uint16_t block_align = 4;
    uint16_t bits = 16;
};

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
    uint64_t base_position = 0;
    Clock::time_point started;
    uint32_t face_buffer = 0;
    uint32_t face_3d = 0;
    uint32_t face_listener = 0;
    std::array<float, 3> position{};
    std::array<float, 3> velocity{};
    float min_distance = 1;
    float max_distance = 1e9f;
    uint32_t mode = 0;

    ~Buffer() { allocator().free(storage); }

    uint32_t bytes_per_second() const {
        uint32_t rate = frequency ? frequency : format.rate;
        return std::max<uint32_t>(1, rate * std::max<uint16_t>(format.block_align, 1));
    }

    uint64_t raw_position() const {
        if (!playing) {
            return base_position;
        }
        double seconds = std::chrono::duration<double>(Clock::now() - started).count();
        return base_position + static_cast<uint64_t>(seconds * bytes_per_second());
    }

    uint32_t play_cursor() {
        if (size == 0) {
            return 0;
        }
        uint64_t raw = raw_position();
        if (!looping && raw >= size) {
            playing = false;
            base_position = 0;
            return 0;
        }
        uint32_t align = std::max<uint16_t>(format.block_align, 1);
        return static_cast<uint32_t>(raw % size) / align * align;
    }

    uint32_t write_cursor() {
        uint32_t p = play_cursor();
        uint32_t align = std::max<uint16_t>(format.block_align, 1);
        uint32_t lead = std::min<uint32_t>(size, bytes_per_second() / 100) / align * align;
        return size ? (p + lead) % size : 0;
    }

    void freeze() {
        if (playing && size != 0) {
            base_position = raw_position() % size;
        }
    }
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
        b->play_cursor();
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
    b->freeze();
    b->looping = (flags & kDsbPlayLooping) != 0 || b->primary;
    b->playing = true;
    b->started = Clock::now();
    return kSOk;
}

uint32_t buf_set_current_position(uint32_t self, uint32_t position) {
    std::lock_guard lock(g_mutex);
    Buffer* b = buf(self);
    if (!b) {
        return kDsErrInvalidParam;
    }
    b->base_position = b->size ? position % b->size : 0;
    b->started = Clock::now();
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
        b->freeze();
        b->started = Clock::now();
        b->frequency = v;
    }
    return kSOk;
}

uint32_t buf_stop(uint32_t self) {
    std::lock_guard lock(g_mutex);
    if (Buffer* b = buf(self)) {
        b->freeze();
        b->playing = false;
    }
    return kSOk;
}

uint32_t buf_unlock(uint32_t self, uint32_t p1, uint32_t b1, uint32_t p2, uint32_t b2) { return kSOk; }
uint32_t buf_restore(uint32_t self) { return kSOk; }

uint32_t b3d_set_position(uint32_t self, float x, float y, float z, uint32_t apply) {
    std::lock_guard lock(g_mutex);
    if (Buffer* b = buf(self)) {
        b->position = {x, y, z};
    }
    return kSOk;
}

uint32_t b3d_set_velocity(uint32_t self, float x, float y, float z, uint32_t apply) {
    std::lock_guard lock(g_mutex);
    if (Buffer* b = buf(self)) {
        b->velocity = {x, y, z};
    }
    return kSOk;
}

uint32_t b3d_set_min_distance(uint32_t self, float d, uint32_t apply) {
    std::lock_guard lock(g_mutex);
    if (Buffer* b = buf(self)) {
        b->min_distance = d;
    }
    return kSOk;
}

uint32_t b3d_set_max_distance(uint32_t self, float d, uint32_t apply) {
    std::lock_guard lock(g_mutex);
    if (Buffer* b = buf(self)) {
        b->max_distance = d;
    }
    return kSOk;
}

uint32_t b3d_set_mode(uint32_t self, uint32_t mode, uint32_t apply) {
    std::lock_guard lock(g_mutex);
    if (Buffer* b = buf(self)) {
        b->mode = mode;
    }
    return kSOk;
}

uint32_t b3d_get_mode(uint32_t self, uint32_t out) {
    std::lock_guard lock(g_mutex);
    Buffer* b = buf(self);
    com_out(out, b ? b->mode : 0);
    return kSOk;
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
                          ComMethod{"SetFX", 4, nullptr, kENotImpl},
                          ComMethod{"AcquireResources", 4, com_noop},
                          ComMethod{"GetObjectInPath", 5, nullptr, kENoInterface},
                      });

ComInterface g_buffer3d("dsound.dll", "IDirectSound3DBuffer",
                        {
                            DS_UNKNOWN(buf_query_interface),
                            ComMethod{"GetAllParameters", 2, com_noop},
                            ComMethod{"GetConeAngles", 3, com_noop},
                            ComMethod{"GetConeOrientation", 2, com_noop},
                            ComMethod{"GetConeOutsideVolume", 2, com_noop},
                            ComMethod{"GetMaxDistance", 2, com_noop},
                            ComMethod{"GetMinDistance", 2, com_noop},
                            com_method<b3d_get_mode>("GetMode"),
                            ComMethod{"GetPosition", 2, com_noop},
                            ComMethod{"GetVelocity", 2, com_noop},
                            ComMethod{"SetAllParameters", 3, com_noop},
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
                            ComMethod{"GetAllParameters", 2, com_noop},
                            ComMethod{"GetDistanceFactor", 2, com_noop},
                            ComMethod{"GetDopplerFactor", 2, com_noop},
                            ComMethod{"GetOrientation", 3, com_noop},
                            ComMethod{"GetPosition", 2, com_noop},
                            ComMethod{"GetRolloffFactor", 2, com_noop},
                            ComMethod{"GetVelocity", 2, com_noop},
                            ComMethod{"SetAllParameters", 3, com_noop},
                            ComMethod{"SetDistanceFactor", 3, com_noop},
                            ComMethod{"SetDopplerFactor", 3, com_noop},
                            ComMethod{"SetOrientation", 8, com_noop},
                            ComMethod{"SetPosition", 5, com_noop},
                            ComMethod{"SetRolloffFactor", 3, com_noop},
                            ComMethod{"SetVelocity", 5, com_noop},
                            ComMethod{"CommitDeferredSettings", 1, com_noop},
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
    std::fprintf(stderr, "[DSOUND] DirectSoundCreate8: silent device (CoreAudio output is M5 work)\n");
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

}
