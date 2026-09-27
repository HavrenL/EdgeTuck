#pragma once
#include <cmath>

namespace edge {
// Windows 11 top-level window/flyout radius, in device-independent pixels.
inline constexpr float drawer_corner_dip = 8.0f;
// Paint inside the existing grid slot; adjacent surfaces leave a 4 DIP gap.
inline constexpr float drawer_visual_inset_dip = 2.0f;
struct GlassMaterial {
    float blur{1.2f}, depth{18}, light{.75f}, dispersion{.55f};
    bool refraction{true}, lighting{true}, chromatic{true};
    bool operator==(const GlassMaterial&) const = default;
};
inline GlassMaterial glass_preset(int index) {
    if (index == 1) return {2.4f, 16, .65f, .4f};
    if (index == 2) return {.45f, 24, 1, .85f};
    return {};
}
inline bool valid_material(const GlassMaterial& v) {
    return std::isfinite(v.blur) && v.blur >= 0 && v.blur <= 6
        && std::isfinite(v.depth) && v.depth >= 0 && v.depth <= 36
        && std::isfinite(v.light) && v.light >= 0 && v.light <= 1.5f
        && std::isfinite(v.dispersion) && v.dispersion >= 0 && v.dispersion <= 2;
}
}
