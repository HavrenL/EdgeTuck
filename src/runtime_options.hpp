#pragma once
#include <windows.h>
#include <filesystem>
#include <string>

namespace edge {
enum class StartupState { Off, On, OtherPath, Error };
struct StartupStatus { StartupState state{StartupState::Off}; LSTATUS error{}; };
std::filesystem::path executable_path();
std::wstring startup_command(const std::filesystem::path& executable);
// The handle-based helpers allow tests to use an isolated, non-startup key.
LSTATUS read_run_entry(HKEY key,std::wstring& command);
LSTATUS write_run_entry(HKEY key,const std::wstring& command,bool enable);
StartupStatus startup_status();
LSTATUS set_startup_enabled(bool enable);
bool set_responsive_priority(bool enable,DWORD& error);
}
