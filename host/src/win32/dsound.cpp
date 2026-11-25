#include "com.hpp"
#include "process.hpp"

#include <cstring>

namespace ma::host::win32 {

namespace {

constexpr uint32_t kSpeakerStereo = 4;

constexpr std::string_view kDirectSound8Methods[] = {
    "IDirectSound8::QueryInterface",       "IDirectSound8::AddRef",
    "IDirectSound8::Release",              "IDirectSound8::CreateSoundBuffer",
    "IDirectSound8::GetCaps",              "IDirectSound8::DuplicateSoundBuffer",
    "IDirectSound8::SetCooperativeLevel",  "IDirectSound8::Compact",
    "IDirectSound8::GetSpeakerConfig",     "IDirectSound8::SetSpeakerConfig",
    "IDirectSound8::Initialize",           "IDirectSound8::VerifyCertification",
};

constexpr ComClass kDirectSound8{"dsound.dll", kDirectSound8Methods};

void create(ApiCall& c) {
    uint32_t out = c.arg(1);
    if (out == 0) {
        c.ret(kEFail);
        return;
    }
    mem::write<uint32_t>(out, com_create(kDirectSound8));
    c.ret(kSOk);
}

constexpr ApiDef kDsound[] = {
    {"dsound.dll", "", 11, 3, Conv::stdcall, create},
    {"dsound.dll", "DirectSoundCreate8", 0, 3, Conv::stdcall, create},
    {"dsound.dll", "IDirectSound8::QueryInterface", 0, 3, Conv::stdcall,
     [](ApiCall& c) {
         if (c.arg(2) != 0) {
             mem::write<uint32_t>(c.arg(2), 0);
         }
         c.ret(kENoInterface);
     }},
    {"dsound.dll", "IDirectSound8::AddRef", 0, 1, Conv::stdcall, [](ApiCall& c) { c.ret(com_add_ref(c.arg(0))); }},
    {"dsound.dll", "IDirectSound8::Release", 0, 1, Conv::stdcall, [](ApiCall& c) { c.ret(com_release(c.arg(0))); }},
    {"dsound.dll", "IDirectSound8::GetCaps", 0, 2, Conv::stdcall,
     [](ApiCall& c) {
         uint32_t caps = c.arg(1);
         uint32_t size = mem::read<uint32_t>(caps);
         std::memset(mem::ptr(caps + 4), 0, size > 4 ? size - 4 : 0);
         c.ret(kSOk);
     }},
    {"dsound.dll", "IDirectSound8::SetCooperativeLevel", 0, 3, Conv::stdcall, [](ApiCall& c) { c.ret(kSOk); }},
    {"dsound.dll", "IDirectSound8::GetSpeakerConfig", 0, 2, Conv::stdcall,
     [](ApiCall& c) {
         mem::write<uint32_t>(c.arg(1), kSpeakerStereo);
         c.ret(kSOk);
     }},
};

}

std::span<const ApiDef> dsound_apis() { return kDsound; }

}
