#include "com_object.hpp"
#include "dshow.hpp"
#include "user32.hpp"

#include "paths.hpp"

#include "../gfx/d3d9.hpp"
#include "../gfx/renderer.hpp"
#include "../input/keys.hpp"
#include "../media/movie.hpp"
#include "../settings.hpp"

#include <algorithm>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>

namespace ma::host::win32 {

namespace {

constexpr uint32_t kEAbort = 0x80004004u;
constexpr uint32_t kEcComplete = 1;
constexpr uint32_t kStateStopped = 0;
constexpr uint32_t kStatePaused = 1;
constexpr uint32_t kStateRunning = 2;

struct Graph;

std::vector<Graph*> g_playing;

void stop_ticking(Graph* g) { std::erase(g_playing, g); }

struct Graph {
    ~Graph() { stop_ticking(this); }

    uint32_t refs = 1;
    std::unique_ptr<media::Movie> movie;
    bool finished = false;
    std::u16string file;
    uint32_t state = kStateStopped;
    uint32_t notify_window = 0;
    uint32_t notify_message = 0;
    uint32_t notify_param = 0;
    std::deque<uint32_t> events;
    uint32_t builder = 0;
    uint32_t control = 0;
    uint32_t event = 0;
    uint32_t seeking = 0;
    uint32_t video = 0;
};

struct Face : ComObject {
    std::shared_ptr<Graph> graph;
};

Graph* graph(uint32_t self) {
    auto* f = com_get<Face>(self);
    return f ? f->graph.get() : nullptr;
}

std::string narrow(const std::u16string& s) {
    std::string out;
    for (char16_t c : s) {
        out.push_back(c < 0x80 ? static_cast<char>(c) : '?');
    }
    return out;
}

uint32_t query_interface(uint32_t self, uint32_t riid, uint32_t out);

uint32_t add_ref(uint32_t self) {
    Graph* g = graph(self);
    return g ? ++g->refs : 0;
}

uint32_t release(uint32_t self) {
    Graph* g = graph(self);
    if (!g || g->refs == 0) {
        return 0;
    }
    if (--g->refs != 0) {
        return g->refs;
    }
    for (uint32_t face : {g->builder, g->control, g->event, g->seeking, g->video}) {
        if (face != 0) {
            com_objects().destroy(face);
        }
    }
    return 0;
}

uint32_t get_type_info_count(uint32_t self, uint32_t out) {
    com_out(out, 0);
    return kSOk;
}

void finish(Graph& g) {
    stop_ticking(&g);
    if (g.finished) {
        return;
    }
    g.finished = true;
    g.events.push_back(kEcComplete);
    if (g.notify_window != 0 && g.notify_message != 0) {
        post_message(g.notify_window, g.notify_message, 0, g.notify_param);
    }
}

uint32_t gb_render_file(uint32_t self, uint32_t file, uint32_t playlist) {
    Graph* g = graph(self);
    if (!g) {
        return kEFail;
    }
    g->file = file ? mem::read_wstr(file) : u"";
    std::string name = narrow(g->file);
    if (settings().skip_intro) {
        std::fprintf(stderr, "[DSHOW] RenderFile(\"%s\"): skipped by --skip-intro\n", name.c_str());
        return kSOk;
    }
    auto host = host_path_for(normalize_guest_path(name, current_directory()));
    g->movie = host ? media::Movie::open(*host) : nullptr;
    if (!g->movie) {
        std::fprintf(stderr, "[DSHOW] RenderFile(\"%s\"): cannot open as MPEG-1, skipping it\n", name.c_str());
        return kSOk;
    }
    std::fprintf(stderr, "[DSHOW] RenderFile(\"%s\"): %ux%u, %.1f s, %s\n", name.c_str(), g->movie->width(),
                 g->movie->height(), g->movie->duration(), g->movie->has_audio() ? "with audio" : "no audio track");
    return kSOk;
}

uint32_t mc_run(uint32_t self) {
    Graph* g = graph(self);
    if (!g) {
        return kEFail;
    }
    g->state = kStateRunning;
    if (!g->movie || g->movie->ended()) {
        finish(*g);
        return kSOk;
    }
    g->movie->play();
    if (std::ranges::find(g_playing, g) == g_playing.end()) {
        g_playing.push_back(g);
    }
    return kSOk;
}

uint32_t mc_pause(uint32_t self) {
    if (Graph* g = graph(self)) {
        g->state = kStatePaused;
        if (g->movie) {
            g->movie->pause();
        }
    }
    return kSOk;
}

uint32_t mc_stop(uint32_t self) {
    if (Graph* g = graph(self)) {
        g->state = kStateStopped;
        if (g->movie) {
            g->movie->pause();
        }
        stop_ticking(g);
    }
    return kSOk;
}

uint32_t mc_get_state(uint32_t self, uint32_t timeout, uint32_t out) {
    Graph* g = graph(self);
    com_out(out, g ? g->state : kStateStopped);
    return kSOk;
}

uint32_t me_get_event_handle(uint32_t self, uint32_t out) {
    com_out(out, 0);
    return kENotImpl;
}

uint32_t me_get_event(uint32_t self, uint32_t code, uint32_t p1, uint32_t p2, uint32_t timeout) {
    Graph* g = graph(self);
    if (!g || g->events.empty()) {
        return kEAbort;
    }
    com_out(code, g->events.front());
    com_out(p1, 0);
    com_out(p2, 0);
    g->events.pop_front();
    return kSOk;
}

uint32_t me_wait_for_completion(uint32_t self, uint32_t timeout, uint32_t code) {
    com_out(code, kEcComplete);
    return kSOk;
}

uint32_t me_free_event_params(uint32_t self, uint32_t code, uint32_t p1, uint32_t p2) { return kSOk; }

uint32_t me_set_notify_window(uint32_t self, uint32_t hwnd, uint32_t msg, uint32_t param) {
    if (Graph* g = graph(self)) {
        g->notify_window = hwnd;
        g->notify_message = msg;
        g->notify_param = param;
    }
    return kSOk;
}

uint32_t me_set_notify_flags(uint32_t self, uint32_t flags) { return kSOk; }

uint32_t me_get_notify_flags(uint32_t self, uint32_t out) {
    com_out(out, 0);
    return kSOk;
}

#define DSHOW_DISPATCH                                                                                             \
    com_method<query_interface>("QueryInterface"), com_method<add_ref>("AddRef"), com_method<release>("Release"), \
        com_method<get_type_info_count>("GetTypeInfoCount"), ComMethod{"GetTypeInfo", 4, nullptr, kENotImpl},    \
        ComMethod{"GetIDsOfNames", 6, nullptr, kENotImpl}, ComMethod{"Invoke", 9, nullptr, kENotImpl}

ComInterface g_builder("quartz.dll", "IGraphBuilder",
                       {
                           com_method<query_interface>("QueryInterface"),
                           com_method<add_ref>("AddRef"),
                           com_method<release>("Release"),
                           ComMethod{"AddFilter", 3, com_noop},
                           ComMethod{"RemoveFilter", 2, com_noop},
                           ComMethod{"EnumFilters", 2, nullptr, kENotImpl},
                           ComMethod{"FindFilterByName", 3, nullptr, kENotImpl},
                           ComMethod{"ConnectDirect", 4, com_noop},
                           ComMethod{"Reconnect", 2, com_noop},
                           ComMethod{"Disconnect", 2, com_noop},
                           ComMethod{"SetDefaultSyncSource", 1, com_noop},
                           ComMethod{"Connect", 3, com_noop},
                           ComMethod{"Render", 2, com_noop},
                           com_method<gb_render_file>("RenderFile"),
                           ComMethod{"AddSourceFilter", 4, nullptr, kENotImpl},
                           ComMethod{"SetLogFile", 2, com_noop},
                           ComMethod{"Abort", 1, com_noop},
                           ComMethod{"ShouldOperationContinue", 1, com_noop},
                       });

ComInterface g_control("quartz.dll", "IMediaControl",
                       {
                           DSHOW_DISPATCH,
                           com_method<mc_run>("Run"),
                           com_method<mc_pause>("Pause"),
                           com_method<mc_stop>("Stop"),
                           com_method<mc_get_state>("GetState"),
                           ComMethod{"RenderFile", 2, nullptr, kENotImpl},
                           ComMethod{"AddSourceFilter", 3, nullptr, kENotImpl},
                           ComMethod{"get_FilterCollection", 2, nullptr, kENotImpl},
                           ComMethod{"get_RegFilterCollection", 2, nullptr, kENotImpl},
                           com_method<mc_stop>("StopWhenReady"),
                       });

ComInterface g_event("quartz.dll", "IMediaEventEx",
                     {
                         DSHOW_DISPATCH,
                         com_method<me_get_event_handle>("GetEventHandle"),
                         com_method<me_get_event>("GetEvent"),
                         com_method<me_wait_for_completion>("WaitForCompletion"),
                         ComMethod{"CancelDefaultHandling", 2, com_noop},
                         ComMethod{"RestoreDefaultHandling", 2, com_noop},
                         com_method<me_free_event_params>("FreeEventParams"),
                         com_method<me_set_notify_window>("SetNotifyWindow"),
                         com_method<me_set_notify_flags>("SetNotifyFlags"),
                         com_method<me_get_notify_flags>("GetNotifyFlags"),
                     });

ComInterface g_seeking("quartz.dll", "IMediaSeeking",
                       {
                           com_method<query_interface>("QueryInterface"),
                           com_method<add_ref>("AddRef"),
                           com_method<release>("Release"),
                           ComMethod{"GetCapabilities", 2, nullptr, kENotImpl},
                           ComMethod{"CheckCapabilities", 2, nullptr, kENotImpl},
                           ComMethod{"IsFormatSupported", 2, nullptr, kENotImpl},
                           ComMethod{"QueryPreferredFormat", 2, nullptr, kENotImpl},
                           ComMethod{"GetTimeFormat", 2, nullptr, kENotImpl},
                           ComMethod{"IsUsingTimeFormat", 2, nullptr, kENotImpl},
                           ComMethod{"SetTimeFormat", 2, nullptr, kENotImpl},
                           ComMethod{"GetDuration", 2, nullptr, kENotImpl},
                           ComMethod{"GetStopPosition", 2, nullptr, kENotImpl},
                           ComMethod{"GetCurrentPosition", 2, nullptr, kENotImpl},
                           ComMethod{"ConvertTimeFormat", 6, nullptr, kENotImpl},
                           ComMethod{"SetPositions", 5, com_noop},
                           ComMethod{"GetPositions", 3, nullptr, kENotImpl},
                           ComMethod{"GetAvailable", 3, nullptr, kENotImpl},
                           ComMethod{"SetRate", 3, com_noop},
                           ComMethod{"GetRate", 2, nullptr, kENotImpl},
                           ComMethod{"GetPreroll", 2, nullptr, kENotImpl},
                       });

ComInterface g_video("quartz.dll", "IVideoWindow",
                     {
                         DSHOW_DISPATCH,
                         ComMethod{"put_Caption", 2, com_noop},          ComMethod{"get_Caption", 2, nullptr, kENotImpl},
                         ComMethod{"put_WindowStyle", 2, com_noop},      ComMethod{"get_WindowStyle", 2, nullptr, kENotImpl},
                         ComMethod{"put_WindowStyleEx", 2, com_noop},    ComMethod{"get_WindowStyleEx", 2, nullptr, kENotImpl},
                         ComMethod{"put_AutoShow", 2, com_noop},         ComMethod{"get_AutoShow", 2, nullptr, kENotImpl},
                         ComMethod{"put_WindowState", 2, com_noop},      ComMethod{"get_WindowState", 2, nullptr, kENotImpl},
                         ComMethod{"put_BackgroundPalette", 2, com_noop}, ComMethod{"get_BackgroundPalette", 2, nullptr, kENotImpl},
                         ComMethod{"put_Visible", 2, com_noop},          ComMethod{"get_Visible", 2, nullptr, kENotImpl},
                         ComMethod{"put_Left", 2, com_noop},             ComMethod{"get_Left", 2, nullptr, kENotImpl},
                         ComMethod{"put_Width", 2, com_noop},            ComMethod{"get_Width", 2, nullptr, kENotImpl},
                         ComMethod{"put_Top", 2, com_noop},              ComMethod{"get_Top", 2, nullptr, kENotImpl},
                         ComMethod{"put_Height", 2, com_noop},           ComMethod{"get_Height", 2, nullptr, kENotImpl},
                         ComMethod{"put_Owner", 2, com_noop},            ComMethod{"get_Owner", 2, nullptr, kENotImpl},
                         ComMethod{"put_MessageDrain", 2, com_noop},     ComMethod{"get_MessageDrain", 2, nullptr, kENotImpl},
                         ComMethod{"get_BorderColor", 2, nullptr, kENotImpl},
                         ComMethod{"put_BorderColor", 2, com_noop},
                         ComMethod{"get_FullScreenMode", 2, nullptr, kENotImpl},
                         ComMethod{"put_FullScreenMode", 2, com_noop},
                         ComMethod{"SetWindowForeground", 2, com_noop},
                         ComMethod{"NotifyOwnerMessage", 5, com_noop},
                         ComMethod{"SetWindowPosition", 5, com_noop},
                         ComMethod{"GetWindowPosition", 5, nullptr, kENotImpl},
                         ComMethod{"GetMinIdealImageSize", 3, nullptr, kENotImpl},
                         ComMethod{"GetMaxIdealImageSize", 3, nullptr, kENotImpl},
                         ComMethod{"GetRestorePosition", 5, nullptr, kENotImpl},
                         ComMethod{"HideCursor", 2, com_noop},
                         ComMethod{"IsCursorHidden", 2, nullptr, kENotImpl},
                     });

bool iid_is(uint32_t riid, uint32_t d1, uint16_t d2, uint16_t d3, std::initializer_list<uint8_t> d4) {
    return guid_equals(riid, d1, d2, d3, d4);
}

bool quartz_iid(uint32_t riid, uint32_t d1) {
    return iid_is(riid, d1, 0x0ad4, 0x11ce, {0xb0, 0x3a, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70});
}

uint32_t query_interface(uint32_t self, uint32_t riid, uint32_t out) {
    Graph* g = graph(self);
    com_out(out, 0);
    if (!g) {
        return kENoInterface;
    }
    uint32_t face = 0;
    if (quartz_iid(riid, 0x56a868a9) || iid_is(riid, 0, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46})) {
        face = g->builder;
    } else if (quartz_iid(riid, 0x56a868b1)) {
        face = g->control;
    } else if (quartz_iid(riid, 0x56a868c0) || quartz_iid(riid, 0x56a868b6)) {
        face = g->event;
    } else if (quartz_iid(riid, 0x56a868b4)) {
        face = g->video;
    } else if (iid_is(riid, 0x36b73880, 0xc2c8, 0x11cf, {0x8b, 0x46, 0x00, 0x80, 0x5f, 0x6c, 0xef, 0x60})) {
        face = g->seeking;
    }
    if (face == 0) {
        std::fprintf(stderr, "[DSHOW] QueryInterface(%s) refused\n", guid_string(riid).c_str());
        return kENoInterface;
    }
    ++g->refs;
    com_out(out, face);
    return kSOk;
}

uint32_t make_face(ComInterface& iface, const std::shared_ptr<Graph>& g) {
    auto f = std::make_unique<Face>();
    f->graph = g;
    return com_objects().wrap(iface, std::move(f));
}

}

void tick_movies() {
    if (g_playing.empty()) {
        return;
    }
    auto playing = g_playing;
    for (Graph* g : playing) {
        if (!g->movie) {
            continue;
        }
        if (g->movie->advance()) {
            if (auto* r = d3d9::active_renderer()) {
                r->present_image(g->movie->pixels(), g->movie->width(), g->movie->height(), g->movie->width() * 4);
                input::run_key_script(true, r->image_frame());
            }
        }
        if (g->movie->ended()) {
            std::fprintf(stderr, "[DSHOW] \"%s\" finished after %llu frames\n", narrow(g->file).c_str(),
                         static_cast<unsigned long long>(g->movie->frames()));
            finish(*g);
        }
    }
}

bool is_filter_graph(uint32_t clsid) {
    return guid_equals(clsid, 0xe436ebb3, 0x524f, 0x11ce, {0x9f, 0x53, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70});
}

uint32_t create_filter_graph(uint32_t riid, uint32_t out) {
    auto g = std::make_shared<Graph>();
    g->builder = make_face(g_builder, g);
    g->control = make_face(g_control, g);
    g->event = make_face(g_event, g);
    g->seeking = make_face(g_seeking, g);
    g->video = make_face(g_video, g);
    uint32_t rc = query_interface(g->builder, riid, out);
    release(g->builder);
    return rc;
}

}
