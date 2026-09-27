#pragma once
#include <windows.h>
#include <filesystem>
#include <string_view>

namespace edge {
void diagnostic_file(const std::filesystem::path& path);
void diagnostic(std::string_view event,HRESULT result=S_OK,HWND window=nullptr) noexcept;
unsigned diagnostic_errors();
}
