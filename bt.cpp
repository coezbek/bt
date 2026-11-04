// Entry point. Windows starts bt.exe for every link click, so this file keeps the forwarding path tiny:
// find the resident process, hand the click over, exit. Only the first start (or --background at sign-in)
// pays for loading config, Lua, ClearURLs and the UI.
#include <windows.h>
#include <shlobj.h>
#include <exdisp.h>
#include <shldisp.h>
#include <initializer_list>
#include <string>
#include "globals.h"
#include "common/str.h"
#include "cmdline.h"
#include "app/request.h"
#include "app/host.h"

using namespace std;
using namespace grey::common;
using namespace bt;

static uint64_t process_start_100ns() {
    FILETIME creation{}, exit{}, kernel{}, user{};
    if(!::GetProcessTimes(::GetCurrentProcess(), &creation, &exit, &kernel, &user)) return 0;
    return (static_cast<uint64_t>(creation.dwHighDateTime) << 32) | creation.dwLowDateTime;
}

static uint64_t now_100ns() {
    FILETIME ft{};
    ::GetSystemTimePreciseAsFileTime(&ft);
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

static uint32_t read_modifiers() {
    uint32_t m{mod_none};
    if(::GetAsyncKeyState(VK_SHIFT) & 0x8000) m |= mod_shift;
    if(::GetAsyncKeyState(VK_CONTROL) & 0x8000) m |= mod_ctrl;
    if(::GetAsyncKeyState(VK_MENU) & 0x8000) m |= mod_alt;
    if(::GetKeyState(VK_CAPITAL) & 1) m |= mod_caps;
    return m;
}

static HWND find_host() {
    return ::FindWindowW(HOST_WINDOW_CLASS, nullptr);
}

/**
 * @brief Sends the request to the resident process. Returns false if there is none or it did not answer.
 */
static bool forward(HWND host, const request& r) {
    // We inherited the right to take the foreground from the app the link was clicked in.
    // Pass it on, so the picker (or the browser the resident process starts) can come to the front.
    DWORD host_pid{0};
    ::GetWindowThreadProcessId(host, &host_pid);
    if(host_pid) ::AllowSetForegroundWindow(host_pid);

    const string payload = r.serialize();
    COPYDATASTRUCT cds{};
    cds.dwData = HOST_COPYDATA_ID;
    cds.cbData = static_cast<DWORD>(payload.size());
    cds.lpData = const_cast<char*>(payload.data());
    DWORD_PTR result{0};
    return ::SendMessageTimeoutW(host, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cds),
        SMTO_ABORTIFHUNG | SMTO_BLOCK, 5000, &result) != 0 && result == 1;
}

/**
 * @brief The command line without the program name. pCmdLine is not reliable across C runtimes.
 */
static wstring args_after_program() {
    const wchar_t* p = ::GetCommandLineW();
    if(*p == L'"') {
        p++;
        while(*p && *p != L'"') p++;
        if(*p) p++;
    } else {
        while(*p && *p != L' ' && *p != L'\t') p++;
    }
    while(*p == L' ' || *p == L'\t') p++;
    return p;
}

static bool is_elevated() {
    HANDLE token{nullptr};
    if(!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION e{};
    DWORD size{0};
    const bool r = ::GetTokenInformation(token, TokenElevation, &e, sizeof(e), &size) && e.TokenIsElevated;
    ::CloseHandle(token);
    return r;
}

/**
 * @brief Starts "bt.exe --background" at normal rights by asking Explorer's desktop to run it.
 *
 * An elevated app (an admin terminal, Visual Studio) passes its elevation to the bt.exe it starts. That
 * process must not become the resident instance, or every later link would open browsers as admin.
 */
static bool start_unelevated_host() {
    bool ok{false};
    if(FAILED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return false;
    IShellWindows* windows{nullptr};
    IDispatch* desktop{nullptr};
    IServiceProvider* provider{nullptr};
    IShellBrowser* browser{nullptr};
    IShellView* view{nullptr};
    IDispatch* background{nullptr};
    IShellFolderViewDual* folder_view{nullptr};
    IDispatch* app{nullptr};
    IShellDispatch2* shell{nullptr};
    VARIANT loc{}, empty{};
    loc.vt = VT_I4;
    loc.lVal = CSIDL_DESKTOP;
    long hwnd{0};
    if(SUCCEEDED(::CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&windows))) &&
       windows->FindWindowSW(&loc, &empty, SWC_DESKTOP, &hwnd, SWFO_NEEDDISPATCH, &desktop) == S_OK &&
       SUCCEEDED(desktop->QueryInterface(IID_PPV_ARGS(&provider))) &&
       SUCCEEDED(provider->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser))) &&
       SUCCEEDED(browser->QueryActiveShellView(&view)) &&
       SUCCEEDED(view->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(&background))) &&
       SUCCEEDED(background->QueryInterface(IID_PPV_ARGS(&folder_view))) &&
       SUCCEEDED(folder_view->get_Application(&app)) &&
       SUCCEEDED(app->QueryInterface(IID_PPV_ARGS(&shell)))) {
        wchar_t exe[MAX_PATH]{};
        ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
        BSTR file = ::SysAllocString(exe);
        VARIANT args{}, dir{}, op{}, show{};
        args.vt = VT_BSTR;
        args.bstrVal = ::SysAllocString(L"--background");
        show.vt = VT_I4;
        show.lVal = SW_SHOWNORMAL;
        ok = SUCCEEDED(shell->ShellExecute(file, args, dir, op, show));
        ::SysFreeString(args.bstrVal);
        ::SysFreeString(file);
    }
    for(IUnknown* p : std::initializer_list<IUnknown*>{shell, app, folder_view, background, view, browser, provider, desktop, windows})
        if(p) p->Release();
    ::CoUninitialize();
    return ok;
}

static string first_word(const string& s, string& rest) {
    size_t pos = s.find(' ');
    if(pos == string::npos) {
        rest.clear();
        return s;
    }
    rest = s.substr(pos + 1);
    str::trim(rest);
    return s.substr(0, pos);
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    request r;
    r.launcher_main_100ns = now_100ns();
    r.launcher_start_100ns = process_start_100ns();
    r.modifiers = read_modifiers();
    r.source_hwnd = reinterpret_cast<uintptr_t>(::GetForegroundWindow());

    string args = str::to_str(args_after_program());
    str::trim(args);
    str::trim(args, "\"");

    string rest;
    const string word = first_word(args, rest);

    if(args.empty()) {
        r.kind = request_kind::config;
    } else if(word == "--background") {
        r.kind = request_kind::background;
    } else if(word == "--exit") {
        r.kind = request_kind::exit;
    } else if(word == "discover") {
        r.kind = request_kind::discover;
    } else if(word == "pick") {
        r.kind = request_kind::pick;
        r.url = rest;
        str::trim(r.url, "\"");
    } else if(word == "browser") {
        // console commands run in this process and never start the UI
        bt_init_core();
        cmdline c;
        return c.exec(word, rest + ArgSplitter);
    } else {
        r.kind = request_kind::open;
        r.url = args;
    }

    if(HWND h = find_host(); h && forward(h, r)) return 0;
    if(r.kind == request_kind::exit) return 0;

    if(is_elevated() && start_unelevated_host()) {
        for(int i = 0; i < 100; i++) {
            if(HWND h = find_host(); h && forward(h, r)) return 0;
            ::Sleep(50);
        }
        // fall through: rather an elevated host than a lost click
    }

    // No resident process yet. Two clicks can race here; the mutex makes one of them the host and the
    // other waits for the host window and forwards to it. If the mutex's owner goes away without ever
    // showing a window (it crashed, or is shutting down), the waiting launcher takes the mutex and
    // becomes the host itself, so the click is not lost.
    HANDLE mutex = ::CreateMutexW(nullptr, TRUE, HOST_MUTEX_NAME);
    if(mutex && ::GetLastError() == ERROR_ALREADY_EXISTS) {
        const ULONGLONG deadline = ::GetTickCount64() + 10000;
        bool owned{false};
        while(::GetTickCount64() < deadline) {
            if(HWND h = find_host(); h && forward(h, r)) {
                ::CloseHandle(mutex);
                return 0;
            }
            const DWORD w = ::WaitForSingleObject(mutex, 100);
            if(w == WAIT_OBJECT_0 || w == WAIT_ABANDONED) {
                // the previous owner released it; a host may have appeared just before
                if(HWND h = find_host(); h && forward(h, r)) {
                    ::ReleaseMutex(mutex);
                    ::CloseHandle(mutex);
                    return 0;
                }
                owned = true;
                break;
            }
        }
        if(!owned) {
            ::CloseHandle(mutex);
            return 1;
        }
    }

    int code = host::run(r);
    if(mutex) {
        ::ReleaseMutex(mutex);
        ::CloseHandle(mutex);
    }
    return code;
}
