#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <functional>
#include <memory>
#include <vector>

namespace edge {
struct DesktopLayer {
    RECT bounds{};
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
};
struct DesktopFrame {
    RECT bounds{};
    std::vector<DesktopLayer> layers; // Desktop hosts only, bottom to top.
};
// Capture desktop hosts, never the final monitor composition. The DWM waiter
// only schedules work; all frame/GPU access stays on the window thread.
class DesktopCapture {
    struct Impl;
    std::unique_ptr<Impl> impl;
public:
    DesktopCapture(ID3D11Device* device, HWND notify, UINT message);
    ~DesktopCapture();
    void active(bool value);
    bool consume(const std::function<void(const DesktopFrame&)>& draw, bool refresh_retained = false);
    HRESULT error() const;
    bool active() const;
    void source_for_test(HWND hwnd);
};
}
