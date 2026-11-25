#pragma once

#include "../thunks.hpp"

#include <span>

namespace ma::gfx {
class Renderer;
}

namespace ma::host::d3d9 {

std::span<const ApiDef> apis();
gfx::Renderer* active_renderer();

}
