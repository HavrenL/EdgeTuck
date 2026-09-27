#pragma once
#include <d2d1_1.h>
#include <d2d1effects.h>
#include <windows.graphics.effects.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Effects.h>
#include <winrt/Windows.UI.Composition.h>
#include <vector>
#include "glass_profile.hpp"

namespace edge {
namespace effects_abi = ABI::Windows::Graphics::Effects;
// HostBackdropBrush provides the live source; the blur is an explicit effect.
// Merely lowering a backdrop visual's opacity leaves sharp desktop text visible.
struct BackdropBlurEffect : winrt::implements<BackdropBlurEffect,
    winrt::Windows::Graphics::Effects::IGraphicsEffect,
    winrt::Windows::Graphics::Effects::IGraphicsEffectSource,
    effects_abi::IGraphicsEffectD2D1Interop> {
    winrt::hstring name{L"LiveGlassBlur"};
    winrt::Windows::Graphics::Effects::IGraphicsEffectSource input{nullptr};
    float deviation{glass_blur_dip};
    winrt::hstring Name() const { return name; }
    void Name(winrt::hstring const& value) { name = value; }
    HRESULT __stdcall GetEffectId(GUID* id) noexcept override { if (!id) return E_POINTER; *id = CLSID_D2D1GaussianBlur; return S_OK; }
    HRESULT __stdcall GetNamedPropertyMapping(LPCWSTR, UINT*, effects_abi::GRAPHICS_EFFECT_PROPERTY_MAPPING*) noexcept override { return E_INVALIDARG; }
    HRESULT __stdcall GetPropertyCount(UINT* count) noexcept override { if (!count) return E_POINTER; *count = 3; return S_OK; }
    HRESULT __stdcall GetProperty(UINT index, ABI::Windows::Foundation::IPropertyValue** value) noexcept override {
        if (!value) return E_POINTER; *value = nullptr;
        try {
            using winrt::Windows::Foundation::PropertyValue;
            winrt::Windows::Foundation::IInspectable property{nullptr};
            if (index == D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION) property = PropertyValue::CreateSingle(deviation);
            else if (index == D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION) property = PropertyValue::CreateUInt32(D2D1_GAUSSIANBLUR_OPTIMIZATION_BALANCED);
            else if (index == D2D1_GAUSSIANBLUR_PROP_BORDER_MODE) property = PropertyValue::CreateUInt32(D2D1_BORDER_MODE_HARD);
            else return E_INVALIDARG;
            return winrt::get_unknown(property)->QueryInterface(__uuidof(ABI::Windows::Foundation::IPropertyValue), reinterpret_cast<void**>(value));
        } catch (...) { return winrt::to_hresult(); }
    }
    HRESULT __stdcall GetSource(UINT index, effects_abi::IGraphicsEffectSource** source) noexcept override {
        if (!source) return E_POINTER; *source = nullptr;
        if (index != 0 || !input) return E_INVALIDARG;
        return winrt::get_unknown(input)->QueryInterface(__uuidof(effects_abi::IGraphicsEffectSource), reinterpret_cast<void**>(source));
    }
    HRESULT __stdcall GetSourceCount(UINT* count) noexcept override { if (!count) return E_POINTER; *count = 1; return S_OK; }
};
// CompositionMaskBrush only accepts surface/color/nine-grid sources. Mask the
// effect inside its own effect graph instead (destination-in composition).
struct MaskedGlassEffect : winrt::implements<MaskedGlassEffect,
    winrt::Windows::Graphics::Effects::IGraphicsEffect,
    winrt::Windows::Graphics::Effects::IGraphicsEffectSource,
    effects_abi::IGraphicsEffectD2D1Interop> {
    winrt::hstring name{L"RimMask"};
    winrt::Windows::Graphics::Effects::IGraphicsEffectSource input{nullptr}, mask{nullptr};
    winrt::hstring Name() const { return name; }
    void Name(winrt::hstring const& value) { name = value; }
    HRESULT __stdcall GetEffectId(GUID* id) noexcept override { if (!id) return E_POINTER; *id = CLSID_D2D1Composite; return S_OK; }
    HRESULT __stdcall GetNamedPropertyMapping(LPCWSTR, UINT*, effects_abi::GRAPHICS_EFFECT_PROPERTY_MAPPING*) noexcept override { return E_INVALIDARG; }
    HRESULT __stdcall GetPropertyCount(UINT* count) noexcept override { if (!count) return E_POINTER; *count = 1; return S_OK; }
    HRESULT __stdcall GetProperty(UINT index, ABI::Windows::Foundation::IPropertyValue** value) noexcept override {
        if (!value) return E_POINTER; *value = nullptr;
        if (index != D2D1_COMPOSITE_PROP_MODE) return E_INVALIDARG;
        try {
            auto property = winrt::Windows::Foundation::PropertyValue::CreateUInt32(D2D1_COMPOSITE_MODE_DESTINATION_IN);
            return winrt::get_unknown(property)->QueryInterface(__uuidof(ABI::Windows::Foundation::IPropertyValue), reinterpret_cast<void**>(value));
        } catch (...) { return winrt::to_hresult(); }
    }
    HRESULT __stdcall GetSource(UINT index, effects_abi::IGraphicsEffectSource** source) noexcept override {
        if (!source) return E_POINTER; *source = nullptr;
        if (index > 1) return E_INVALIDARG;
        const auto& value = index == 0 ? input : mask;
        return value ? winrt::get_unknown(value)->QueryInterface(__uuidof(effects_abi::IGraphicsEffectSource), reinterpret_cast<void**>(source)) : E_INVALIDARG;
    }
    HRESULT __stdcall GetSourceCount(UINT* count) noexcept override { if (!count) return E_POINTER; *count = 2; return S_OK; }
};
}
