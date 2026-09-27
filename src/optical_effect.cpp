#include "optical_effect.hpp"
#include <d2d1effectauthor.h>
#include <d2d1effecthelpers.h>
#include <wrl/client.h>
#include <cstring>
#include <new>
#include <algorithm>
#include <cmath>
#include "optical_shader.h"

namespace edge {
static constexpr GUID optical_shader_id = {0x72d56b61,0x23db,0x476d,{0xb3,0xb8,0x19,0x5e,0x27,0x50,0xa2,0x34}};
class OpticalEffect final : public ID2D1EffectImpl, public ID2D1DrawTransform {
    LONG refs{1};
    Microsoft::WRL::ComPtr<ID2D1DrawInfo> draw;
    OpticalConstants constants;
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** value) override {
        if (!value) return E_POINTER;
        *value = nullptr;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(ID2D1EffectImpl)) *value = static_cast<ID2D1EffectImpl*>(this);
        else if (iid == __uuidof(ID2D1DrawTransform) || iid == __uuidof(ID2D1Transform) || iid == __uuidof(ID2D1TransformNode)) *value = static_cast<ID2D1DrawTransform*>(this);
        else return E_NOINTERFACE;
        AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
    ULONG STDMETHODCALLTYPE Release() override { const ULONG n = InterlockedDecrement(&refs); if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE Initialize(ID2D1EffectContext* context, ID2D1TransformGraph* graph) override {
        const HRESULT hr = context->LoadPixelShader(optical_shader_id, edge_optical_shader, sizeof(edge_optical_shader));
        return FAILED(hr) ? hr : graph->SetSingleTransformNode(this);
    }
    HRESULT STDMETHODCALLTYPE PrepareForRender(D2D1_CHANGE_TYPE) override {
        return draw ? draw->SetPixelShaderConstantBuffer(reinterpret_cast<const BYTE*>(&constants), sizeof(constants)) : E_UNEXPECTED;
    }
    HRESULT STDMETHODCALLTYPE SetGraph(ID2D1TransformGraph*) override { return E_NOTIMPL; }
    UINT32 STDMETHODCALLTYPE GetInputCount() const override { return 2; }
    HRESULT STDMETHODCALLTYPE MapOutputRectToInputRects(const D2D1_RECT_L* output, D2D1_RECT_L* inputs, UINT32 count) const override {
        if (count != 2) return E_INVALIDARG;
        // For eta=1/1.48 the shader's lateral displacement is < 2*depth.
        // GaussianBlur expands this request for its own kernel upstream.
        const LONG margin=static_cast<LONG>(std::ceil((2*constants.depth+constants.dispersion)*constants.scale))+2;
        for (UINT32 i=0;i<count;++i) inputs[i] = {std::max(0L,output->left-margin),std::max(0L,output->top-margin),
            std::min(static_cast<LONG>(constants.width),output->right+margin),std::min(static_cast<LONG>(constants.height),output->bottom+margin)};
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE MapInputRectsToOutputRect(const D2D1_RECT_L*, const D2D1_RECT_L*, UINT32 count, D2D1_RECT_L* output, D2D1_RECT_L* opaque) override {
        if (count != 2) return E_INVALIDARG;
        *output = {0,0,static_cast<LONG>(constants.width),static_cast<LONG>(constants.height)};
        *opaque = {}; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE MapInvalidRect(UINT32, D2D1_RECT_L, D2D1_RECT_L* output) const override {
        *output = {0,0,static_cast<LONG>(constants.width),static_cast<LONG>(constants.height)}; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetDrawInfo(ID2D1DrawInfo* info) override {
        draw = info;
        const D2D1_INPUT_DESCRIPTION description{D2D1_FILTER_MIN_MAG_MIP_LINEAR,0};
        HRESULT hr = info->SetInputDescription(0, description);
        if (SUCCEEDED(hr)) hr = info->SetInputDescription(1, description);
        return FAILED(hr) ? hr : info->SetPixelShader(optical_shader_id);
    }
    HRESULT SetConstants(const BYTE* data, UINT32 size) {
        if (!size) { constants = {}; return S_OK; }
        if (size != sizeof(constants) || !data) return E_INVALIDARG;
        std::memcpy(&constants, data, size); return S_OK;
    }
    HRESULT GetConstants(BYTE* data, UINT32 size, UINT32* actual) const {
        if (actual) *actual = sizeof(constants);
        if (!data) return S_OK;
        if (size < sizeof(constants)) return E_NOT_SUFFICIENT_BUFFER;
        std::memcpy(data, &constants, sizeof(constants)); return S_OK;
    }
    static HRESULT CALLBACK Create(IUnknown** result) {
        auto* effect = new(std::nothrow) OpticalEffect();
        if (!effect) return E_OUTOFMEMORY;
        *result = static_cast<ID2D1EffectImpl*>(effect); return S_OK;
    }
};
HRESULT register_optical_effect(ID2D1Factory1* factory) {
    const D2D1_PROPERTY_BINDING bindings[] = {D2D1_BLOB_TYPE_BINDING(L"Constants", &OpticalEffect::SetConstants, &OpticalEffect::GetConstants)};
    return factory->RegisterEffectFromString(CLSID_EdgeOptical,
        LR"(<?xml version='1.0'?><Effect><Property name='DisplayName' type='string' value='EdgeTuck Optical Glass'/><Property name='Author' type='string' value='EdgeTuck'/><Property name='Category' type='string' value='Material'/><Property name='Description' type='string' value='Blurred scene refraction and a single luminous rim'/><Inputs><Input name='Blurred'/><Input name='Scattered'/></Inputs><Property name='Constants' type='blob'><Property name='DisplayName' type='string' value='Constants'/><Property name='Default' type='blob' value=''/></Property></Effect>)",
        bindings, ARRAYSIZE(bindings), OpticalEffect::Create);
}
}

