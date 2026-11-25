#include "seh.hpp"

#include "process.hpp"

#include "../crash.hpp"
#include "../dispatch.hpp"
#include "../guest_memory.hpp"

#include <atomic>
#include <cstring>
#include <cstdio>

namespace ma::host::win32 {

namespace {

std::atomic<uint32_t> g_unhandled_filter = 0;

uint32_t teb_of(GuestState& s) { return s.addr.fs_base.dword; }

bool frame_valid(GuestState& s, uint32_t frame) {
    uint32_t teb = teb_of(s);
    uint32_t base = mem::read<uint32_t>(teb + 4);
    uint32_t limit = mem::read<uint32_t>(teb + 8);
    return (frame & 3) == 0 && frame >= limit && frame + 8 <= base;
}

uint32_t call_handler(GuestState& s, uint32_t handler, uint32_t record, uint32_t frame, uint32_t context,
                      uint32_t dispatcher) {
    uint32_t saved_esp = esp(s);
    uint32_t saved_eip = eip(s);
    const uint32_t args[] = {record, frame, context, dispatcher};
    for (int i = 3; i >= 0; --i) {
        esp(s) -= 4;
        mem::write<uint32_t>(esp(s), args[i]);
    }
    esp(s) -= 4;
    mem::write<uint32_t>(esp(s), kReturnSentinel);
    dispatch(s, handler);
    if (eip(s) != kReturnSentinel) {
        throw GuestUnwind{eip(s), esp(s)};
    }
    esp(s) = saved_esp;
    eip(s) = saved_eip;
    return eax(s);
}

}

uint32_t seh_head(GuestState& s) { return mem::read<uint32_t>(teb_of(s)); }

void set_seh_head(GuestState& s, uint32_t frame) { mem::write<uint32_t>(teb_of(s), frame); }

void write_exception_record(uint32_t record, uint32_t code, uint32_t flags, uint32_t address, uint32_t nested,
                            const uint32_t* params, uint32_t count) {
    std::memset(mem::ptr(record), 0, kExceptionRecordSize);
    if (count > 15) {
        count = 15;
    }
    mem::write<uint32_t>(record + 0, code);
    mem::write<uint32_t>(record + 4, flags);
    mem::write<uint32_t>(record + 8, nested);
    mem::write<uint32_t>(record + 12, address);
    mem::write<uint32_t>(record + 16, count);
    for (uint32_t i = 0; i < count; ++i) {
        mem::write<uint32_t>(record + 20 + 4 * i, params[i]);
    }
}

void capture_context(GuestState& s, uint32_t context, uint32_t eip_value, uint32_t esp_value) {
    std::memset(mem::ptr(context), 0, kContextSize);
    mem::write<uint32_t>(context + context_offset::flags, kContextFull);
    mem::write<uint32_t>(context + context_offset::edi, edi(s));
    mem::write<uint32_t>(context + context_offset::esi, esi(s));
    mem::write<uint32_t>(context + context_offset::ebx, ebx(s));
    mem::write<uint32_t>(context + context_offset::edx, edx(s));
    mem::write<uint32_t>(context + context_offset::ecx, ecx(s));
    mem::write<uint32_t>(context + context_offset::eax, eax(s));
    mem::write<uint32_t>(context + context_offset::ebp, ebp(s));
    mem::write<uint32_t>(context + context_offset::eip, eip_value);
    mem::write<uint32_t>(context + context_offset::cs, 0x1b);
    mem::write<uint32_t>(context + context_offset::eflags, 0x202);
    mem::write<uint32_t>(context + context_offset::esp, esp_value);
    mem::write<uint32_t>(context + context_offset::ss, 0x23);
}

void restore_context(GuestState& s, uint32_t context) {
    edi(s) = mem::read<uint32_t>(context + context_offset::edi);
    esi(s) = mem::read<uint32_t>(context + context_offset::esi);
    ebx(s) = mem::read<uint32_t>(context + context_offset::ebx);
    edx(s) = mem::read<uint32_t>(context + context_offset::edx);
    ecx(s) = mem::read<uint32_t>(context + context_offset::ecx);
    eax(s) = mem::read<uint32_t>(context + context_offset::eax);
    ebp(s) = mem::read<uint32_t>(context + context_offset::ebp);
}

DispatchResult dispatch_exception(GuestState& s, uint32_t record, uint32_t context) {
    uint32_t saved_esp = esp(s);
    esp(s) -= 16;
    uint32_t dispatcher = esp(s);
    mem::write<uint32_t>(dispatcher, 0);
    uint32_t frame = seh_head(s);
    while (frame != kSehChainEnd) {
        if (!frame_valid(s, frame)) {
            std::fprintf(stderr, "[SEH] invalid registration 0x%08x in the fs:[0] chain\n", frame);
            break;
        }
        uint32_t handler = mem::read<uint32_t>(frame + 4);
        auto disposition = static_cast<Disposition>(call_handler(s, handler, record, frame, context, dispatcher));
        switch (disposition) {
        case Disposition::continue_execution:
            if (mem::read<uint32_t>(record + 4) & kExceptionNoncontinuable) {
                std::fprintf(stderr, "[SEH] handler 0x%08x continued a noncontinuable exception\n", handler);
                esp(s) = saved_esp;
                return DispatchResult::unhandled;
            }
            esp(s) = saved_esp;
            return DispatchResult::continued;
        case Disposition::continue_search:
            break;
        default:
            std::fprintf(stderr, "[SEH] handler 0x%08x returned unsupported disposition %u\n", handler,
                         static_cast<uint32_t>(disposition));
            esp(s) = saved_esp;
            return DispatchResult::unhandled;
        }
        frame = mem::read<uint32_t>(frame);
    }
    esp(s) = saved_esp;
    return DispatchResult::unhandled;
}

void unwind_to(GuestState& s, uint32_t target_frame, uint32_t record) {
    uint32_t saved_esp = esp(s);
    esp(s) -= kExceptionRecordSize + kContextSize + 16;
    esp(s) &= ~15u;
    uint32_t local_record = esp(s);
    uint32_t context = local_record + kExceptionRecordSize;
    uint32_t dispatcher = context + kContextSize;
    if (record == 0) {
        write_exception_record(local_record, kStatusUnwind, 0, eip(s), 0, nullptr, 0);
        record = local_record;
    }
    uint32_t flags = mem::read<uint32_t>(record + 4) | kExceptionUnwinding;
    if (target_frame == 0) {
        flags |= kExceptionExitUnwind;
    }
    mem::write<uint32_t>(record + 4, flags);
    capture_context(s, context, eip(s), saved_esp);
    mem::write<uint32_t>(dispatcher, 0);

    uint32_t frame = seh_head(s);
    while (frame != kSehChainEnd && frame != target_frame) {
        if (!frame_valid(s, frame)) {
            std::fprintf(stderr, "[SEH] RtlUnwind hit invalid registration 0x%08x\n", frame);
            break;
        }
        if (target_frame != 0 && target_frame < frame) {
            std::fprintf(stderr, "[SEH] RtlUnwind target 0x%08x is below registration 0x%08x\n", target_frame, frame);
            break;
        }
        uint32_t handler = mem::read<uint32_t>(frame + 4);
        call_handler(s, handler, record, frame, context, dispatcher);
        uint32_t next = mem::read<uint32_t>(frame);
        set_seh_head(s, next);
        frame = next;
    }
    esp(s) = saved_esp;
}

uint32_t unhandled_exception_filter() { return g_unhandled_filter.load(); }

uint32_t set_unhandled_exception_filter(uint32_t filter) { return g_unhandled_filter.exchange(filter); }

bool unwind_matches_call(const GuestUnwind& unwind, uint32_t return_slot, uint32_t return_address) {
    if (unwind.eip != return_address || unwind.esp <= return_slot) {
        return false;
    }
    uint32_t popped = unwind.esp - return_slot - 4;
    return popped <= 0x100 && (popped & 3) == 0;
}

namespace {

constexpr uint32_t kExceptionContinueExecution = 0xffffffffu;

void raise_exception(ApiCall& c) {
    GuestState& s = c.s;
    uint32_t code = c.arg(0);
    uint32_t flags = c.arg(1) & kExceptionNoncontinuable;
    uint32_t count = c.arg(2);
    uint32_t params_addr = c.arg(3);
    uint32_t return_address = mem::read<uint32_t>(esp(s));
    uint32_t caller_esp = esp(s) + 4 + 16;

    uint32_t params[15] = {};
    if (count > 15) {
        count = 15;
    }
    for (uint32_t i = 0; i < count && params_addr != 0; ++i) {
        params[i] = mem::read<uint32_t>(params_addr + 4 * i);
    }

    uint32_t saved_esp = esp(s);
    esp(s) -= kExceptionRecordSize + kContextSize + 16;
    esp(s) &= ~15u;
    uint32_t record = esp(s);
    uint32_t context = record + kExceptionRecordSize;
    uint32_t pointers = context + kContextSize;
    write_exception_record(record, code, flags, return_address, 0, params, count);
    capture_context(s, context, return_address, caller_esp);
    mem::write<uint32_t>(pointers, record);
    mem::write<uint32_t>(pointers + 4, context);

    std::fprintf(stderr, "[EXCEPTION] RaiseException code=0x%08x flags=0x%x from 0x%08x\n", code, flags,
                 return_address);
    auto result = dispatch_exception(s, record, context);
    if (result == DispatchResult::unhandled) {
        if (uint32_t filter = unhandled_exception_filter()) {
            if (call_guest(s, filter, {pointers}, true) == kExceptionContinueExecution) {
                result = DispatchResult::continued;
            }
        }
    }
    if (result == DispatchResult::unhandled) {
        std::fprintf(stderr, "[EXCEPTION] unhandled exception 0x%08x raised from 0x%08x\n", code, return_address);
        print_host_backtrace();
        exit_process(code);
    }
    if (mem::read<uint32_t>(context + context_offset::eip) != return_address ||
        mem::read<uint32_t>(context + context_offset::esp) != caller_esp) {
        std::fprintf(stderr, "[EXCEPTION] handler redirected execution to 0x%08x esp=0x%08x, which is unsupported\n",
                     mem::read<uint32_t>(context + context_offset::eip),
                     mem::read<uint32_t>(context + context_offset::esp));
        guest_fatal(s, "SEH", return_address);
    }
    restore_context(s, context);
    esp(s) = saved_esp;
}

void rtl_unwind(ApiCall& c) {
    GuestState& s = c.s;
    uint32_t target_frame = c.arg(0);
    uint32_t target_ip = c.arg(1);
    uint32_t record = c.arg(2);
    uint32_t value = c.arg(3);
    uint32_t return_address = mem::read<uint32_t>(esp(s));
    uint32_t saved[] = {ebx(s), esi(s), edi(s), ebp(s)};
    unwind_to(s, target_frame, record);
    ebx(s) = saved[0];
    esi(s) = saved[1];
    edi(s) = saved[2];
    ebp(s) = saved[3];
    if (target_ip != 0 && target_ip != return_address) {
        std::fprintf(stderr, "[SEH] RtlUnwind target ip 0x%08x differs from return address 0x%08x\n", target_ip,
                     return_address);
        guest_fatal(s, "SEH", target_ip);
    }
    c.ret(value);
}

constexpr ApiDef kSeh[] = {
    {"KERNEL32.dll", "RaiseException", 0, 4, Conv::stdcall, raise_exception},
    {"KERNEL32.dll", "RtlUnwind", 0, 4, Conv::stdcall, rtl_unwind},
    {"KERNEL32.dll", "SetUnhandledExceptionFilter", 0, 1, Conv::stdcall,
     [](ApiCall& c) { c.ret(set_unhandled_exception_filter(c.arg(0))); }},
    {"KERNEL32.dll", "UnhandledExceptionFilter", 0, 1, Conv::stdcall,
     [](ApiCall& c) {
         uint32_t filter = unhandled_exception_filter();
         c.ret(filter ? call_guest(c.s, filter, {c.arg(0)}, true) : 1);
     }},
};

}

std::span<const ApiDef> seh_apis() { return kSeh; }

}
