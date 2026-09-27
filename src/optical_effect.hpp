#pragma once
#include <d2d1_1.h>
#include "material.hpp"

namespace edge {
inline constexpr GUID CLSID_EdgeOptical = {0x95e6068a,0x77b9,0x4dbb,{0xaa,0x45,0x80,0x71,0x47,0xc9,0x5f,0x31}};
struct OpticalConstants {
    float width{}, height{}, scale{1}, radius{drawer_corner_dip};
    float depth{18}, light{.75f}, dispersion{.55f}, surface_inset{drawer_visual_inset_dip};
    float refraction{1}, lighting{1}, chromatic{1}, reserved{};
};
HRESULT register_optical_effect(ID2D1Factory1* factory);
}
