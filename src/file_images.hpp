#pragma once
#include <windows.h>
#include <memory>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#ifdef EDGETUCK_IMAGE_TESTS
#include <functional>
#endif

namespace edge {
inline constexpr UINT file_image_ready=WM_APP+24;
struct FileImage {
    unsigned width{},height{};
    bool thumbnail{};
    std::vector<BYTE> pixels; // Premultiplied BGRA, independent of any GPU device.
};
enum class ImagePriority { Visible, Background };
inline int image_pixels(float dip,float dpi) {
    // Subtract only floating point coordinate noise at integral pixel sizes.
    return std::clamp(static_cast<int>(std::ceil(dip*dpi/96.0f-.001f)),16,256);
}
// Shell extraction is performed on one lazy worker, never during painting.
class FileImages {
    struct State;
    std::shared_ptr<State> state;
public:
    FileImages();
#ifdef EDGETUCK_IMAGE_TESTS
    explicit FileImages(std::function<std::shared_ptr<const FileImage>(const std::wstring&,int)> loader);
#endif
    ~FileImages();
    std::shared_ptr<const FileImage> request(const std::wstring& path,int pixels,HWND repaint,
        ImagePriority priority=ImagePriority::Visible);
    void forget(const std::wstring& path);
};
}
