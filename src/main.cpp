#include "app.hpp"
#include "diagnostics.hpp"
#include "menu_host.hpp"
#include <commctrl.h>
#include <shellapi.h>
#include <string>
#include <string_view>
#include <iostream>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    // Parse quoted arguments and trailing whitespace consistently. In particular,
    // redirected test launches must never silently fall into the normal profile.
    int count=0; LPWSTR* values=CommandLineToArgvW(GetCommandLineW(),&count);
    if(!values) return 1;
    const std::wstring argument=count==2?values[1]:L""; LocalFree(values);
    if(count>2) return 1;
    std::wstring_view args(argument);
    const auto first=args.find_first_not_of(L" \t\r\n");
    if(first==std::wstring_view::npos) args={};
    else { args.remove_prefix(first); args=args.substr(0,args.find_last_not_of(L" \t\r\n")+1); }
    if(args.starts_with(L"--shell-menu=")) return edge::file_menu_host(args);
    if(!args.empty() && args!=L"--diagnostics" && args!=L"--quit" && args!=L"--material-preview"
        && args!=L"--smoke-test" && args!=L"--smoke-fallback" && args!=L"--tray" && args!=L"--live-preview" && args!=L"--smoke-live" && args!=L"--smoke-stress" && args!=L"--smoke-archive") return 1;
    if (args == L"--diagnostics") {
        try {
            const auto path = edge::default_config_path();
            std::wcout << L"Config: " << path.wstring() << L"\nExists: " << std::filesystem::exists(path) << L'\n';
            edge::Settings settings; std::wstring error;
            const bool loaded = edge::load_settings(path, settings, error);
            std::wcout << L"Load: " << loaded << L"\nDrawers: " << settings.drawers.size() << L'\n';
            const auto probe = std::filesystem::path(path.parent_path() / (L"write-probe-" + std::to_wstring(GetCurrentProcessId()) + L".dat"));
            const bool writable = edge::save_settings(probe, settings, error);
            std::wcout << L"Write probe: " << writable << L"\nError: " << error << L'\n';
            if (writable) std::filesystem::remove(probe);
            return loaded && writable ? 0 : 1;
        } catch (...) { return 1; }
    }
    if (args == L"--quit") {
        if (HWND broker = FindWindowW(L"EdgeTuck.Broker", L"EdgeTuck Broker")) PostMessageW(broker, edge::WM_EDGE_SHUTDOWN, 0, 0);
        return 0;
    }
    const bool material_preview = args == L"--material-preview" || args==L"--live-preview";
    const bool smoke = args == L"--smoke-test" || args == L"--smoke-fallback" || args==L"--smoke-live" || args==L"--smoke-stress" || args==L"--smoke-archive";
    HANDLE mutex = nullptr;
    if (!smoke && !material_preview) {
        mutex = CreateMutexW(nullptr, TRUE, L"Local\\EdgeTuck.Desktop.Prototype");
        if (!mutex) return 1;
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            if(args!=L"--tray") if (HWND broker = FindWindowW(L"EdgeTuck.Broker", L"EdgeTuck Broker")) PostMessageW(broker, edge::WM_EDGE_SHOW, 0, 0);
            CloseHandle(mutex); return 0;
        }
    }
    const HRESULT ole = OleInitialize(nullptr);
    if (FAILED(ole)) { if (mutex) CloseHandle(mutex); return 1; }
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES}; InitCommonControlsEx(&controls);
    int result = 1;
    try {
        if(!smoke && !material_preview) edge::diagnostic_file(edge::default_config_path().parent_path()/L"logs"/L"runtime.log");
        edge::diagnostic("application.start");
        edge::App app(instance);
        app.stress_test=args==L"--smoke-stress";
        app.archive_test=args==L"--smoke-archive";
        result = app.run(smoke, args == L"--tray", args == L"--smoke-fallback", material_preview, args==L"--live-preview", args==L"--smoke-live" || app.stress_test);
        edge::diagnostic("application.exit");
    } catch (const std::exception&) {
        if (!smoke) MessageBoxW(nullptr, L"轻屉未能启动。请确认使用 Windows 11，并检查图形驱动是否正常。", L"轻屉 · EdgeTuck", MB_OK | MB_ICONERROR);
        result = 1;
    }
    OleUninitialize();
    if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
    return result;
}
