#include "output.hpp"
#include "stream.hpp"

#include "../settings.hpp"

#import <AppKit/AppKit.h>
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <dispatch/dispatch.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ma::host::audio {

namespace {

constexpr uint32_t kRate = 48000;
constexpr uint32_t kFallbackFrames = kRate / 100;

struct Capture {
    std::mutex mutex;
    FILE* file = nullptr;
    uint64_t frames = 0;
    std::vector<int16_t> scratch;
};

struct Output {
    std::mutex mutex;
    bool started = false;
    std::atomic<RenderFn> render{nullptr};
    OutputOptions options;
    AudioUnit unit = nullptr;
    bool suspended = false;
    std::thread fallback;
    std::atomic<bool> fallback_run{false};
    dispatch_queue_t queue = nullptr;
    Capture capture;
    std::atomic<uint64_t> rendered{0};
};

Output& out() {
    static Output* o = new Output;
    return *o;
}

void write_u32(FILE* f, uint32_t v) { std::fwrite(&v, 4, 1, f); }
void write_u16(FILE* f, uint16_t v) { std::fwrite(&v, 2, 1, f); }

void write_wav_header(FILE* f, uint64_t frames) {
    uint32_t data = static_cast<uint32_t>(std::min<uint64_t>(frames * 4, 0xffffffffu - 36));
    std::fseek(f, 0, SEEK_SET);
    std::fwrite("RIFF", 1, 4, f);
    write_u32(f, 36 + data);
    std::fwrite("WAVEfmt ", 1, 8, f);
    write_u32(f, 16);
    write_u16(f, 1);
    write_u16(f, 2);
    write_u32(f, kRate);
    write_u32(f, kRate * 4);
    write_u16(f, 4);
    write_u16(f, 16);
    std::fwrite("data", 1, 4, f);
    write_u32(f, data);
}

void produce(float* buffer, uint32_t frames) {
    Output& o = out();
    std::memset(buffer, 0, sizeof(float) * 2 * frames);
    if (RenderFn render = o.render.load()) {
        render(buffer, frames, kRate);
    }
    mix_streams(buffer, frames, kRate);
    for (uint32_t i = 0; i < 2 * frames; ++i) {
        buffer[i] = std::clamp(buffer[i], -1.0f, 1.0f);
    }
    o.rendered.fetch_add(frames, std::memory_order_relaxed);
    Capture& c = o.capture;
    std::lock_guard lock(c.mutex);
    if (!c.file) {
        return;
    }
    c.scratch.resize(static_cast<size_t>(frames) * 2);
    for (size_t i = 0; i < c.scratch.size(); ++i) {
        c.scratch[i] = static_cast<int16_t>(std::lrint(std::clamp(buffer[i], -1.0f, 1.0f) * 32767.0f));
    }
    std::fwrite(c.scratch.data(), sizeof(int16_t), c.scratch.size(), c.file);
    c.frames += frames;
}

OSStatus render_callback(void*, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, UInt32 frames,
                         AudioBufferList* io) {
    if (!io || io->mNumberBuffers == 0 || !io->mBuffers[0].mData) {
        return noErr;
    }
    auto* buffer = static_cast<float*>(io->mBuffers[0].mData);
    uint32_t capacity = io->mBuffers[0].mDataByteSize / (2 * sizeof(float));
    produce(buffer, std::min<uint32_t>(frames, capacity));
    return noErr;
}

AudioObjectID default_device() {
    AudioObjectPropertyAddress addr{kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    AudioObjectID device = kAudioObjectUnknown;
    UInt32 size = sizeof(device);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &size, &device) != noErr) {
        return kAudioObjectUnknown;
    }
    return device;
}

std::string device_name(AudioObjectID device) {
    AudioObjectPropertyAddress addr{kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    CFStringRef name = nullptr;
    UInt32 size = sizeof(name);
    if (AudioObjectGetPropertyData(device, &addr, 0, nullptr, &size, &name) != noErr || !name) {
        return "unknown device";
    }
    char text[256] = {};
    CFStringGetCString(name, text, sizeof(text), kCFStringEncodingUTF8);
    CFRelease(name);
    return text;
}

void close_unit(Output& o) {
    if (!o.unit) {
        return;
    }
    AudioOutputUnitStop(o.unit);
    AudioUnitUninitialize(o.unit);
    AudioComponentInstanceDispose(o.unit);
    o.unit = nullptr;
}

bool open_unit(Output& o) {
    AudioObjectID device = default_device();
    if (device == kAudioObjectUnknown) {
        std::fprintf(stderr, "[AUDIO] no output device; mixing against the system clock\n");
        return false;
    }
    AudioComponentDescription desc{kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput,
                                   kAudioUnitManufacturer_Apple, 0, 0};
    AudioComponent component = AudioComponentFindNext(nullptr, &desc);
    if (!component || AudioComponentInstanceNew(component, &o.unit) != noErr) {
        o.unit = nullptr;
        return false;
    }
    AudioStreamBasicDescription format{};
    format.mSampleRate = kRate;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    format.mBytesPerPacket = 8;
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = 8;
    format.mChannelsPerFrame = 2;
    format.mBitsPerChannel = 32;
    AURenderCallbackStruct callback{render_callback, nullptr};
    OSStatus status = AudioUnitSetProperty(o.unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &format,
                                           sizeof(format));
    if (status == noErr) {
        status = AudioUnitSetProperty(o.unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0,
                                      &callback, sizeof(callback));
    }
    if (status == noErr) {
        status = AudioUnitInitialize(o.unit);
    }
    if (status == noErr) {
        status = AudioOutputUnitStart(o.unit);
    }
    if (status != noErr) {
        std::fprintf(stderr, "[AUDIO] cannot start output on %s (status %d)\n", device_name(device).c_str(),
                     static_cast<int>(status));
        close_unit(o);
        return false;
    }
    std::fprintf(stderr, "[AUDIO] output on %s, %u Hz stereo\n", device_name(device).c_str(), kRate);
    return true;
}

void fallback_loop() {
    Output& o = out();
    std::vector<float> scratch(static_cast<size_t>(kFallbackFrames) * 2);
    auto next = std::chrono::steady_clock::now();
    while (o.fallback_run.load()) {
        produce(scratch.data(), kFallbackFrames);
        next += std::chrono::milliseconds(10);
        auto now = std::chrono::steady_clock::now();
        if (next < now - std::chrono::milliseconds(200)) {
            next = now;
        }
        std::this_thread::sleep_until(next);
    }
}

void stop_fallback(Output& o) {
    if (o.fallback.joinable()) {
        o.fallback_run = false;
        o.fallback.join();
    }
}

void start_fallback(Output& o) {
    if (o.fallback.joinable()) {
        return;
    }
    o.fallback_run = true;
    o.fallback = std::thread(fallback_loop);
}

void reopen_locked(Output& o) {
    close_unit(o);
    if (o.suspended || o.options.clockless) {
        stop_fallback(o);
        return;
    }
    if (!o.options.silent && open_unit(o)) {
        stop_fallback(o);
    } else {
        start_fallback(o);
    }
}

void reopen(const char* why) {
    Output& o = out();
    std::lock_guard lock(o.mutex);
    if (!o.started) {
        return;
    }
    std::fprintf(stderr, "[AUDIO] %s\n", why);
    reopen_locked(o);
}

void set_suspended(bool suspended) {
    Output& o = out();
    std::lock_guard lock(o.mutex);
    if (!o.started || o.suspended == suspended) {
        return;
    }
    o.suspended = suspended;
    std::fprintf(stderr, "[AUDIO] %s\n", suspended ? "system sleeping; output stopped" : "system awake; output restarted");
    reopen_locked(o);
}

void watch_system(Output& o) {
    o.queue = dispatch_queue_create("MacAndreas.audio", DISPATCH_QUEUE_SERIAL);
    if (o.options.silent || o.options.clockless) {
        return;
    }
    AudioObjectPropertyAddress addr{kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    AudioObjectAddPropertyListenerBlock(kAudioObjectSystemObject, &addr, o.queue,
                                        ^(UInt32, const AudioObjectPropertyAddress*) {
                                          reopen("default output device changed");
                                        });
    NSNotificationCenter* center = NSWorkspace.sharedWorkspace.notificationCenter;
    [center addObserverForName:NSWorkspaceWillSleepNotification
                        object:nil
                         queue:nil
                    usingBlock:^(NSNotification*) {
                      dispatch_sync(out().queue, ^{
                        set_suspended(true);
                      });
                    }];
    [center addObserverForName:NSWorkspaceDidWakeNotification
                        object:nil
                         queue:nil
                    usingBlock:^(NSNotification*) {
                      dispatch_async(out().queue, ^{
                        set_suspended(false);
                      });
                    }];
}

}

void start_output(RenderFn render, const OutputOptions& options) {
    Output& o = out();
    std::lock_guard lock(o.mutex);
    if (render) {
        o.render = render;
    }
    if (o.started) {
        return;
    }
    o.started = true;
    o.options = options;
    std::atexit(finish_output);
    if (!options.capture.empty()) {
        std::lock_guard capture_lock(o.capture.mutex);
        o.capture.file = std::fopen(options.capture.c_str(), "wb");
        if (o.capture.file) {
            write_wav_header(o.capture.file, 0);
            std::fprintf(stderr, "[AUDIO] capturing the mix to %s\n", options.capture.c_str());
        } else {
            std::fprintf(stderr, "[AUDIO] cannot open %s for capture\n", options.capture.c_str());
        }
    }
    watch_system(o);
    if (options.silent) {
        std::fprintf(stderr, "[AUDIO] silent output; mixing against the system clock\n");
    }
    reopen_locked(o);
}

void finish_output() {
    Output& o = out();
    {
        std::lock_guard lock(o.mutex);
        close_unit(o);
        stop_fallback(o);
    }
    Capture& c = o.capture;
    std::lock_guard lock(c.mutex);
    if (c.file) {
        write_wav_header(c.file, c.frames);
        std::fclose(c.file);
        c.file = nullptr;
        std::fprintf(stderr, "[AUDIO] captured %.1f s of audio\n", static_cast<double>(c.frames) / kRate);
    }
}

uint32_t output_rate() { return kRate; }

OutputOptions default_options() {
    return {settings().silent_audio, settings().audio_clockless, settings().audio_capture};
}

}
