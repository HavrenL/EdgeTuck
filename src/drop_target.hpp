#pragma once
#include <windows.h>
#include <oleidl.h>
#include <shobjidl.h>
#include <wrl/client.h>

namespace edge {
class Drawer;
class DropTarget final : public IDropTarget {
    LONG references{1};
    Drawer& drawer;
    bool accepted{};
    bool internal{};
    bool holding_interaction{};
    Microsoft::WRL::ComPtr<IDropTargetHelper> drag_image;
    bool image_entered{};
    DWORD allowed{}, chosen{};
    void update_feedback(DWORD keys);
    void clear_feedback();
public:
    explicit DropTarget(Drawer& drawer);
    ~DropTarget();
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD keys, POINTL point, DWORD* effect) override;
    HRESULT STDMETHODCALLTYPE DragOver(DWORD keys, POINTL point, DWORD* effect) override;
    HRESULT STDMETHODCALLTYPE DragLeave() override;
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD keys, POINTL point, DWORD* effect) override;
};
}
