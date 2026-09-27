#include "runtime_options.hpp"
#include <vector>
#include <shellapi.h>

namespace edge {
static constexpr wchar_t run_key[]=L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static constexpr wchar_t value_name[]=L"EdgeTuck";
std::filesystem::path executable_path() {
    std::vector<wchar_t> buffer(32768);
    const DWORD length=GetModuleFileNameW(nullptr,buffer.data(),static_cast<DWORD>(buffer.size()));
    return length && length<buffer.size()?std::filesystem::path(std::wstring(buffer.data(),length)):std::filesystem::path{};
}
std::wstring startup_command(const std::filesystem::path& executable) {
    const auto path=executable.wstring();
    if(!executable.is_absolute() || path.empty() || path.find_first_of(L"\"\r\n")!=std::wstring::npos || path.find(L'\0')!=std::wstring::npos) return {};
    const auto command=L"\""+path+L"\" --tray";
    // Run entries have a documented 260-character command-line limit.
    return command.size()<260?command:std::wstring{};
}
LSTATUS read_run_entry(HKEY key,std::wstring& command) {
    command.clear(); DWORD size{};
    auto error=RegGetValueW(key,nullptr,value_name,RRF_RT_REG_SZ,nullptr,nullptr,&size);
    if(error==ERROR_FILE_NOT_FOUND) return ERROR_SUCCESS;
    if(error!=ERROR_SUCCESS) return error;
    if(size<sizeof(wchar_t) || size>32768*sizeof(wchar_t)) return ERROR_INVALID_DATA;
    std::vector<wchar_t> value(size/sizeof(wchar_t)+1,L'\0');
    error=RegGetValueW(key,nullptr,value_name,RRF_RT_REG_SZ,nullptr,value.data(),&size);
    if(error==ERROR_SUCCESS) command=value.data();
    return error;
}
LSTATUS write_run_entry(HKEY key,const std::wstring& command,bool enable) {
    if(!enable) { const auto error=RegDeleteValueW(key,value_name); return error==ERROR_FILE_NOT_FOUND?ERROR_SUCCESS:error; }
    if(command.empty() || command.size()>=260 || command.find(L'\0')!=std::wstring::npos) return ERROR_INVALID_PARAMETER;
    return RegSetValueExW(key,value_name,0,REG_SZ,reinterpret_cast<const BYTE*>(command.c_str()),static_cast<DWORD>((command.size()+1)*sizeof(wchar_t)));
}
static bool owned_command(const std::wstring& command) {
    int count{}; auto arguments=CommandLineToArgvW(command.c_str(),&count); bool owned=false;
    if(arguments) {
        if(count==2 && wcscmp(arguments[1],L"--tray")==0) {
            const auto name=std::filesystem::path(arguments[0]).filename().wstring();
            owned=CompareStringOrdinal(name.c_str(),-1,L"EdgeTuck.exe",-1,TRUE)==CSTR_EQUAL;
        }
        LocalFree(arguments);
    }
    return owned;
}
StartupStatus startup_status() {
    HKEY key{}; auto error=RegOpenKeyExW(HKEY_CURRENT_USER,run_key,0,KEY_QUERY_VALUE,&key);
    if(error==ERROR_FILE_NOT_FOUND) return {};
    if(error!=ERROR_SUCCESS) return {StartupState::Error,error};
    std::wstring command; error=read_run_entry(key,command); RegCloseKey(key);
    if(error!=ERROR_SUCCESS) return {StartupState::Error,error};
    if(command.empty()) return {};
    const auto expected=startup_command(executable_path());
    if(!expected.empty() && CompareStringOrdinal(command.c_str(),-1,expected.c_str(),-1,TRUE)==CSTR_EQUAL) return {StartupState::On,0};
    return owned_command(command)?StartupStatus{StartupState::OtherPath,0}:StartupStatus{StartupState::Error,ERROR_ALREADY_EXISTS};
}
LSTATUS set_startup_enabled(bool enable) {
    const auto status=startup_status(); if(status.state==StartupState::Error) return status.error;
    const auto command=startup_command(executable_path());
    if(enable && command.empty()) return ERROR_FILENAME_EXCED_RANGE;
    HKEY key{}; LSTATUS error;
    if(enable) error=RegCreateKeyExW(HKEY_CURRENT_USER,run_key,0,nullptr,0,KEY_SET_VALUE,nullptr,&key,nullptr);
    else error=RegOpenKeyExW(HKEY_CURRENT_USER,run_key,0,KEY_SET_VALUE,&key);
    if(!enable && error==ERROR_FILE_NOT_FOUND) return ERROR_SUCCESS;
    if(error!=ERROR_SUCCESS) return error;
    error=write_run_entry(key,command,enable); RegCloseKey(key); return error;
}
bool set_responsive_priority(bool enable,DWORD& error) {
    error=ERROR_SUCCESS;
    if(SetPriorityClass(GetCurrentProcess(),enable?ABOVE_NORMAL_PRIORITY_CLASS:NORMAL_PRIORITY_CLASS)) return true;
    error=GetLastError(); return false;
}
}
