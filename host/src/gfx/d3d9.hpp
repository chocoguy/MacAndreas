#pragma once

#include "../thunks.hpp"

#include <span>

namespace ma::gfx {
class Renderer;
}

namespace ma::host::d3d9 {

std::span<const ApiDef> apis();
gfx::Renderer* active_renderer();
void set_offset_2d_x(float x);
float offset_2d_x();
void set_mask_widening(float inset, float canvas_width, float height);

}
