#include "audio/output.hpp"

#include <chrono>
#include <cstdio>
#include <thread>

namespace {

int g_calls = 0;

void render(float*, uint32_t, uint32_t) { ++g_calls; }

}

int main(int argc, char** argv) {
    ma::host::audio::OutputOptions options;
    options.silent = true;
    if (argc > 1) {
        options.capture = argv[1];
    }
    ma::host::audio::start_output(render, options);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (g_calls == 0) {
        std::fprintf(stderr, "silent clock never rendered\n");
        return 1;
    }
    std::fprintf(stdout, "audio_exit: rendered %d blocks, returning from main\n", g_calls);
    return 0;
}
