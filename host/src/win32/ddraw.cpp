#include "com.hpp"
#include "process.hpp"

#include <cstdio>
#include <cstring>

namespace ma::host::win32 {

namespace {

constexpr uint32_t kVideoMemory = 0x20000000u;

constexpr std::string_view kDirectDraw7Methods[] = {
    "IDirectDraw7::QueryInterface",
    "IDirectDraw7::AddRef",
    "IDirectDraw7::Release",
    "IDirectDraw7::Compact",
    "IDirectDraw7::CreateClipper",
    "IDirectDraw7::CreatePalette",
    "IDirectDraw7::CreateSurface",
    "IDirectDraw7::DuplicateSurface",
    "IDirectDraw7::EnumDisplayModes",
    "IDirectDraw7::EnumSurfaces",
    "IDirectDraw7::FlipToGDISurface",
    "IDirectDraw7::GetCaps",
    "IDirectDraw7::GetDisplayMode",
    "IDirectDraw7::GetFourCCCodes",
    "IDirectDraw7::GetGDISurface",
    "IDirectDraw7::GetMonitorFrequency",
    "IDirectDraw7::GetScanLine",
    "IDirectDraw7::GetVerticalBlankStatus",
    "IDirectDraw7::Initialize",
    "IDirectDraw7::RestoreDisplayMode",
    "IDirectDraw7::SetCooperativeLevel",
    "IDirectDraw7::SetDisplayMode",
    "IDirectDraw7::WaitForVerticalBlank",
    "IDirectDraw7::GetAvailableVidMem",
    "IDirectDraw7::GetSurfaceFromDC",
    "IDirectDraw7::RestoreAllSurfaces",
    "IDirectDraw7::TestCooperativeLevel",
    "IDirectDraw7::GetDeviceIdentifier",
    "IDirectDraw7::StartModeTest",
    "IDirectDraw7::EvaluateMode",
};

constexpr ComClass kDirectDraw7{"ddraw.dll", kDirectDraw7Methods};

constexpr ApiDef kDdraw[] = {
    {"ddraw.dll", "DirectDrawCreateEx", 0, 4, Conv::stdcall,
     [](ApiCall& c) {
         uint32_t out = c.arg(1);
         if (out == 0) {
             c.ret(kEFail);
             return;
         }
         mem::write<uint32_t>(out, com_create(kDirectDraw7));
         c.ret(kSOk);
     }},
    {"ddraw.dll", "IDirectDraw7::QueryInterface", 0, 3, Conv::stdcall,
     [](ApiCall& c) {
         if (c.arg(2) != 0) {
             mem::write<uint32_t>(c.arg(2), 0);
         }
         c.ret(kENoInterface);
     }},
    {"ddraw.dll", "IDirectDraw7::AddRef", 0, 1, Conv::stdcall, [](ApiCall& c) { c.ret(com_add_ref(c.arg(0))); }},
    {"ddraw.dll", "IDirectDraw7::Release", 0, 1, Conv::stdcall, [](ApiCall& c) { c.ret(com_release(c.arg(0))); }},
    {"ddraw.dll", "IDirectDraw7::GetAvailableVidMem", 0, 4, Conv::stdcall,
     [](ApiCall& c) {
         if (c.arg(2) != 0) {
             mem::write<uint32_t>(c.arg(2), kVideoMemory);
         }
         if (c.arg(3) != 0) {
             mem::write<uint32_t>(c.arg(3), kVideoMemory);
         }
         c.ret(kSOk);
     }},
    {"ddraw.dll", "IDirectDraw7::GetCaps", 0, 3, Conv::stdcall,
     [](ApiCall& c) {
         for (uint32_t caps : {c.arg(1), c.arg(2)}) {
             if (caps != 0) {
                 uint32_t size = mem::read<uint32_t>(caps);
                 std::memset(mem::ptr(caps + 4), 0, size > 4 ? size - 4 : 0);
             }
         }
         c.ret(kSOk);
     }},
    {"ddraw.dll", "IDirectDraw7::GetDeviceIdentifier", 0, 3, Conv::stdcall,
     [](ApiCall& c) {
         uint32_t id = c.arg(1);
         std::memset(mem::ptr(id), 0, 1072);
         mem::write_cstr(id, "macandreas");
         mem::write_cstr(id + 512, "MacAndreas Metal");
         c.ret(kSOk);
     }},
};

}

std::span<const ApiDef> ddraw_apis() { return kDdraw; }

}
