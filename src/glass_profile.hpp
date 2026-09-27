#pragma once
#include <algorithm>
#include <cmath>
#include "material.hpp"

namespace edge {
inline constexpr float glass_blur_dip = 5.5f;
inline constexpr float glass_bend_dip = 8.0f;
inline constexpr float glass_rim_dip = 18.0f;
struct GlassProfile { float x{}, y{}, coverage{}; };
// Signed distance and normal of the SAME rounded rectangle as the visual clip.
// All four sides converge on the corner normal; nothing scales the whole image.
inline GlassProfile glass_profile(float x, float y, float width, float height, float scale) {
    const float radius = drawer_corner_dip*scale;
    const float px = x-width*.5f, py = y-height*.5f;
    const float inset=drawer_visual_inset_dip*scale;
    const float qx = std::abs(px)-(width*.5f-inset-radius), qy = std::abs(py)-(height*.5f-inset-radius);
    const float ox = std::max(qx, 0.0f), oy = std::max(qy, 0.0f);
    const float length = std::hypot(ox, oy);
    const float distance = length + std::min(std::max(qx, qy), 0.0f)-radius;
    if (distance > 0 || distance <= -glass_rim_dip*scale) return {};
    float nx{}, ny{};
    if (length > .0001f) { nx = ox/length; ny = oy/length; }
    else if (qx > qy) nx = 1; else ny = 1;
    nx *= px < 0 ? -1 : 1; ny *= py < 0 ? -1 : 1;
    const float t = std::clamp(-distance/(glass_rim_dip*scale), 0.0f, 1.0f);
    const float coverage = 1-t*t*(3-2*t);
    const float bend = glass_bend_dip*scale*(1-t)*(1-t);
    return {-nx*bend, -ny*bend, coverage};
}
}
