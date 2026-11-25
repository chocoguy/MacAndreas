#include "crash.hpp"

#include "guest_memory.hpp"
#include "thread.hpp"

#include <dlfcn.h>
#include <execinfo.h>
#include <signal.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ma::host {

namespace {

void report_frame(void* pc) {
    Dl_info info{};
    if (dladdr(pc, &info) && info.dli_sname) {
        const char* name = info.dli_sname;
        if (std::strncmp(name, "sub_", 4) == 0) {
            std::fprintf(stderr, "    guest %s\n", name);
        } else {
            std::fprintf(stderr, "    host  %s\n", name);
        }
    } else {
        std::fprintf(stderr, "    ?     %p\n", pc);
    }
}

void on_fault(int sig, siginfo_t* info, void*) {
    auto fault = reinterpret_cast<uintptr_t>(info->si_addr);
    auto base = reinterpret_cast<uintptr_t>(ma_guest_base);
    std::fprintf(stderr, "\n[CRASH] signal %d", sig);
    if (base != 0 && fault >= base && fault < base + kGuestSpan) {
        std::fprintf(stderr, " at guest address 0x%08x", static_cast<uint32_t>(fault - base));
    } else {
        std::fprintf(stderr, " at host address %p", info->si_addr);
    }
    if (auto* t = current_thread()) {
        std::fprintf(stderr, " (thread %u, eip~0x%08x, esp=0x%08x, eax=0x%08x, ecx=0x%08x)", t->id, eip(t->state),
                     esp(t->state), eax(t->state), ecx(t->state));
    }
    std::fprintf(stderr, "\n");
    print_host_backtrace();
    std::fflush(stderr);
    std::_Exit(128 + sig);
}

}

void print_host_backtrace() {
    void* frames[64];
    int n = backtrace(frames, 64);
    std::fprintf(stderr, "  backtrace (innermost first):\n");
    for (int i = 0; i < n; ++i) {
        report_frame(frames[i]);
    }
}

void install_crash_handler() {
    static char alt_stack[1 << 16];
    stack_t ss{};
    ss.ss_sp = alt_stack;
    ss.ss_size = sizeof alt_stack;
    sigaltstack(&ss, nullptr);
    struct sigaction sa{};
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    sigaction(SIGILL, &sa, nullptr);
    sigaction(SIGFPE, &sa, nullptr);
}

}
