#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace ma::host::platform {

struct ContentRect {
    double x = 0;
    double y = 0;
    double width = 0;
    double height = 0;
};

inline ContentRect fit_content(double view_width, double view_height, uint32_t client_width, uint32_t client_height) {
    if (view_width <= 0 || view_height <= 0 || client_width == 0 || client_height == 0) {
        return {0, 0, std::max(view_width, 0.0), std::max(view_height, 0.0)};
    }
    double scale = std::min(view_width / client_width, view_height / client_height);
    double w = client_width * scale;
    double h = client_height * scale;
    return {(view_width - w) / 2, (view_height - h) / 2, w, h};
}

struct ClientPoint {
    double x = 0;
    double y = 0;
};

inline ClientPoint view_to_client(const ContentRect& r, double x, double y_from_bottom, uint32_t client_width,
                                  uint32_t client_height) {
    if (r.width <= 0 || r.height <= 0) {
        return {};
    }
    return {(x - r.x) / r.width * client_width, (r.y + r.height - y_from_bottom) / r.height * client_height};
}

inline ClientPoint client_to_view(const ContentRect& r, double x, double y, uint32_t client_width,
                                  uint32_t client_height) {
    if (client_width == 0 || client_height == 0) {
        return {};
    }
    return {r.x + x * r.width / client_width, r.y + r.height - y * r.height / client_height};
}

inline ClientPoint clamp_client(ClientPoint p, uint32_t client_width, uint32_t client_height) {
    double max_x = client_width > 0 ? client_width - 1 : 0;
    double max_y = client_height > 0 ? client_height - 1 : 0;
    return {std::clamp(p.x, 0.0, max_x), std::clamp(p.y, 0.0, max_y)};
}

}
