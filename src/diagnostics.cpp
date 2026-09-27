#include "diagnostics.hpp"
#include <fstream>
#include <iostream>
#include <mutex>
#include <atomic>
#include <iomanip>

namespace edge {
namespace {
std::mutex log_mutex;
std::filesystem::path log_path;
std::atomic<unsigned> errors{};
}
void diagnostic_file(const std::filesystem::path& path) { std::lock_guard lock(log_mutex); log_path=path; }
unsigned diagnostic_errors() { return errors; }
void diagnostic(std::string_view event,HRESULT result,HWND window) noexcept {
    if(FAILED(result)) ++errors;
    try {
        std::lock_guard lock(log_mutex);
        std::ofstream file;
        if(!log_path.empty()) {
            std::filesystem::create_directories(log_path.parent_path());
            std::error_code ec;
            if(std::filesystem::file_size(log_path,ec)>=512*1024 && !ec) {
                auto previous=log_path; previous+=L".previous";
                // Only this logger's fixed two files are rotated.
                std::filesystem::remove(previous,ec); ec.clear();
                std::filesystem::rename(log_path,previous,ec);
                if(ec) return;
            }
            file.open(log_path,std::ios::app);
        }
        std::ostream& output=file.is_open()?file:std::cout;
        SYSTEMTIME time{}; GetLocalTime(&time);
        output<<time.wYear<<'-'<<time.wMonth<<'-'<<time.wDay<<' '<<time.wHour<<':'<<time.wMinute<<':'<<time.wSecond<<'.'<<time.wMilliseconds
            <<" pid="<<GetCurrentProcessId()<<' '<<event<<" hr=0x"<<std::hex<<static_cast<unsigned long>(result)<<" hwnd=0x"<<reinterpret_cast<UINT_PTR>(window)<<std::dec<<std::endl;
    } catch(...) {}
}
}
