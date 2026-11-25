#include "process.hpp"

#include "../abi.hpp"
#include "../thread.hpp"

#include <vorbis/vorbisfile.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace ma::host::win32 {

namespace {

constexpr uint32_t kScratchSize = 64 * 1024;
constexpr uint32_t kInfoSize = 32;

struct Stream {
    OggVorbis_File vf{};
    uint32_t datasource = 0;
    uint32_t read_fn = 0;
    uint32_t seek_fn = 0;
    uint32_t close_fn = 0;
    uint32_t tell_fn = 0;
    uint32_t scratch = 0;
    uint32_t info = 0;
};

std::mutex g_mutex;
std::unordered_map<uint32_t, std::unique_ptr<Stream>> g_streams;
std::atomic<uint64_t> g_opened{0};

Stream* find(uint32_t vf) {
    std::lock_guard lock(g_mutex);
    auto it = g_streams.find(vf);
    return it == g_streams.end() ? nullptr : it->second.get();
}

GuestState& guest() { return current_thread()->state; }

size_t read_cb(void* ptr, size_t size, size_t nmemb, void* source) {
    auto* st = static_cast<Stream*>(source);
    size_t total = size * nmemb;
    size_t done = 0;
    while (done < total) {
        auto chunk = static_cast<uint32_t>(std::min<size_t>(total - done, kScratchSize));
        uint32_t got = guest_call<Conv::cdecl_, uint32_t>(guest(), st->read_fn, st->scratch, 1u, chunk, st->datasource);
        got = std::min(got, chunk);
        if (got == 0) {
            break;
        }
        std::memcpy(static_cast<uint8_t*>(ptr) + done, mem::ptr(st->scratch), got);
        done += got;
        if (got < chunk) {
            break;
        }
    }
    return size == 0 ? 0 : done / size;
}

int seek_cb(void* source, ogg_int64_t offset, int whence) {
    auto* st = static_cast<Stream*>(source);
    return guest_call<Conv::cdecl_, int32_t>(guest(), st->seek_fn, st->datasource, static_cast<int64_t>(offset),
                                             static_cast<int32_t>(whence));
}

int close_cb(void* source) {
    auto* st = static_cast<Stream*>(source);
    return guest_call<Conv::cdecl_, int32_t>(guest(), st->close_fn, st->datasource);
}

long tell_cb(void* source) {
    auto* st = static_cast<Stream*>(source);
    return guest_call<Conv::cdecl_, int32_t>(guest(), st->tell_fn, st->datasource);
}

void release(Stream& st) {
    allocator().free(st.scratch);
    allocator().free(st.info);
    st.scratch = 0;
    st.info = 0;
}

int32_t open_callbacks(uint32_t datasource, uint32_t vf, uint32_t initial, int32_t ibytes, uint32_t read_fn,
                       uint32_t seek_fn, uint32_t close_fn, uint32_t tell_fn) {
    if (vf == 0 || read_fn == 0) {
        return OV_EFAULT;
    }
    auto st = std::make_unique<Stream>();
    st->datasource = datasource;
    st->read_fn = read_fn;
    st->seek_fn = seek_fn;
    st->close_fn = close_fn;
    st->tell_fn = tell_fn;
    st->scratch = allocator().alloc(kScratchSize);
    std::vector<char> head;
    if (initial != 0 && ibytes > 0) {
        head.assign(reinterpret_cast<const char*>(mem::ptr(initial)),
                    reinterpret_cast<const char*>(mem::ptr(initial)) + ibytes);
    }
    ov_callbacks callbacks{read_cb, seek_fn ? seek_cb : nullptr, close_fn ? close_cb : nullptr,
                           tell_fn ? tell_cb : nullptr};
    int r = ov_open_callbacks(st.get(), &st->vf, head.empty() ? nullptr : head.data(), static_cast<long>(head.size()),
                              callbacks);
    if (r < 0) {
        std::fprintf(stderr, "[VORBIS] ov_open_callbacks(vf=0x%08x) failed: %d\n", vf, r);
        release(*st);
        return r;
    }
    if (g_opened.fetch_add(1) == 0) {
        vorbis_info* vi = ov_info(&st->vf, -1);
        std::fprintf(stderr, "[VORBIS] first stream: %d channel(s), %ld Hz, %.1f s\n", vi ? vi->channels : 0,
                     vi ? vi->rate : 0, ov_time_total(&st->vf, -1));
    }
    std::unique_ptr<Stream> old;
    {
        std::lock_guard lock(g_mutex);
        old = std::exchange(g_streams[vf], std::move(st));
    }
    if (old) {
        ov_clear(&old->vf);
        release(*old);
    }
    return 0;
}

int32_t clear(uint32_t vf) {
    std::unique_ptr<Stream> st;
    {
        std::lock_guard lock(g_mutex);
        auto it = g_streams.find(vf);
        if (it == g_streams.end()) {
            return 0;
        }
        st = std::move(it->second);
        g_streams.erase(it);
    }
    ov_clear(&st->vf);
    release(*st);
    return 0;
}

double time_total(uint32_t vf, int32_t link) {
    Stream* st = find(vf);
    return st ? ov_time_total(&st->vf, link) : OV_EINVAL;
}

double time_tell(uint32_t vf) {
    Stream* st = find(vf);
    return st ? ov_time_tell(&st->vf) : OV_EINVAL;
}

int32_t read(uint32_t vf, uint32_t buffer, int32_t length, int32_t bigendian, int32_t word, int32_t sgned,
             uint32_t bitstream) {
    Stream* st = find(vf);
    if (!st || buffer == 0 || length <= 0) {
        return OV_EINVAL;
    }
    int current = 0;
    long r = ov_read(&st->vf, reinterpret_cast<char*>(mem::ptr(buffer)), length, bigendian, word, sgned, &current);
    if (bitstream != 0) {
        mem::write<int32_t>(bitstream, current);
    }
    return static_cast<int32_t>(r);
}

uint32_t info(uint32_t vf, int32_t link) {
    Stream* st = find(vf);
    if (!st) {
        return 0;
    }
    vorbis_info* vi = ov_info(&st->vf, link);
    if (!vi) {
        return 0;
    }
    if (st->info == 0) {
        st->info = allocator().alloc(kInfoSize);
    }
    mem::write<int32_t>(st->info, vi->version);
    mem::write<int32_t>(st->info + 4, vi->channels);
    mem::write<int32_t>(st->info + 8, static_cast<int32_t>(vi->rate));
    mem::write<int32_t>(st->info + 12, static_cast<int32_t>(vi->bitrate_upper));
    mem::write<int32_t>(st->info + 16, static_cast<int32_t>(vi->bitrate_nominal));
    mem::write<int32_t>(st->info + 20, static_cast<int32_t>(vi->bitrate_lower));
    mem::write<int32_t>(st->info + 24, static_cast<int32_t>(vi->bitrate_window));
    mem::write<uint32_t>(st->info + 28, 0);
    return st->info;
}

int32_t time_seek(uint32_t vf, double seconds) {
    Stream* st = find(vf);
    return st ? ov_time_seek(&st->vf, seconds) : OV_EINVAL;
}

const ApiDef kVorbisFile[] = {
    native_export<Conv::cdecl_, open_callbacks>("vorbisfile.dll", "ov_open_callbacks"),
    native_export<Conv::cdecl_, clear>("vorbisfile.dll", "ov_clear"),
    native_export<Conv::cdecl_, time_total>("vorbisfile.dll", "ov_time_total"),
    native_export<Conv::cdecl_, time_tell>("vorbisfile.dll", "ov_time_tell"),
    native_export<Conv::cdecl_, read>("vorbisfile.dll", "ov_read"),
    native_export<Conv::cdecl_, info>("vorbisfile.dll", "ov_info"),
    native_export<Conv::cdecl_, time_seek>("vorbisfile.dll", "ov_time_seek"),
};

}

std::span<const ApiDef> vorbisfile_apis() { return kVorbisFile; }

}
