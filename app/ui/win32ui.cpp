#include "win32ui.h"
#include <map>
#include <deque>
#include <tuple>
#include <algorithm>
#include <shobjidl.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <commctrl.h>
#include <dwmapi.h>

using namespace std;

namespace bt::ui {

    wstring wide(const string& s) {
        if(s.empty()) return {};
        int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        wstring r(n, L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), r.data(), n);
        return r;
    }

    string narrow(const wstring& s) {
        if(s.empty()) return {};
        int n = ::WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
        string r(n, '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), r.data(), n, nullptr, nullptr);
        return r;
    }

    UINT dpi_of(HWND hwnd) {
        UINT dpi = hwnd ? ::GetDpiForWindow(hwnd) : 0;
        return dpi ? dpi : ::GetDpiForSystem();
    }

    UINT dpi_of_point(POINT pt) {
        HMONITOR mon = ::MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
        UINT x{96}, y{96};
        if(FAILED(::GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &x, &y))) return ::GetDpiForSystem();
        return x;
    }

    RECT cursor_work_area(UINT* dpi) {
        POINT pt{};
        ::GetCursorPos(&pt);
        HMONITOR mon = ::MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{sizeof(mi)};
        ::GetMonitorInfoW(mon, &mi);
        if(dpi) *dpi = dpi_of_point(pt);
        return mi.rcWork;
    }

    static LOGFONTW message_logfont(UINT dpi) {
        NONCLIENTMETRICSW ncm{sizeof(ncm)};
        ::SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi);
        return ncm.lfMessageFont;
    }

    HFONT ui_font(UINT dpi, int size_delta_pt, bool bold) {
        static map<tuple<UINT, int, bool>, HFONT> cache;
        auto key = make_tuple(dpi, size_delta_pt, bold);
        if(auto it = cache.find(key); it != cache.end()) return it->second;

        LOGFONTW lf = message_logfont(dpi);
        if(size_delta_pt != 0) {
            // lfHeight is negative character height in pixels
            int pt = MulDiv(-lf.lfHeight, 72, static_cast<int>(dpi)) + size_delta_pt;
            pt = max(pt, 5);
            lf.lfHeight = -MulDiv(pt, static_cast<int>(dpi), 72);
        }
        if(bold) lf.lfWeight = FW_SEMIBOLD;
        lf.lfQuality = CLEARTYPE_QUALITY;
        HFONT f = ::CreateFontIndirectW(&lf);
        cache[key] = f;
        return f;
    }

    HFONT mono_font(UINT dpi) {
        static map<UINT, HFONT> cache;
        if(auto it = cache.find(dpi); it != cache.end()) return it->second;
        LOGFONTW lf{};
        lf.lfHeight = -MulDiv(10, static_cast<int>(dpi), 72);
        lf.lfQuality = CLEARTYPE_QUALITY;
        wcscpy_s(lf.lfFaceName, L"Consolas");
        HFONT f = ::CreateFontIndirectW(&lf);
        cache[dpi] = f;
        return f;
    }

    static bool font_exists(const wchar_t* face) {
        HDC hdc = ::GetDC(nullptr);
        LOGFONTW lf{};
        lf.lfCharSet = DEFAULT_CHARSET;
        wcscpy_s(lf.lfFaceName, face);
        bool found{false};
        ::EnumFontFamiliesExW(hdc, &lf, [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM p) -> int {
            *reinterpret_cast<bool*>(p) = true;
            return 0;
        }, reinterpret_cast<LPARAM>(&found), 0);
        ::ReleaseDC(nullptr, hdc);
        return found;
    }

    HFONT icon_font(UINT dpi, int size_pt) {
        static map<pair<UINT, int>, HFONT> cache;
        static const wchar_t* face = font_exists(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
        auto key = make_pair(dpi, size_pt);
        if(auto it = cache.find(key); it != cache.end()) return it->second;
        LOGFONTW lf{};
        lf.lfHeight = -MulDiv(size_pt, static_cast<int>(dpi), 72);
        lf.lfQuality = CLEARTYPE_QUALITY;
        wcscpy_s(lf.lfFaceName, face);
        HFONT f = ::CreateFontIndirectW(&lf);
        cache[key] = f;
        return f;
    }

    bool system_uses_dark_theme() {
        DWORD value{1}, size{sizeof(value)};
        if(::RegGetValueW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS) return false;
        return value == 0;
    }

    void apply_dark_title_bar(HWND hwnd, bool dark) {
        BOOL v = dark ? TRUE : FALSE;
        ::DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &v, sizeof(v));
    }

    void apply_round_corners(HWND hwnd) {
        int pref = 2; // DWMWCP_ROUND, Windows 11 only, ignored elsewhere
        ::DwmSetWindowAttribute(hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &pref, sizeof(pref));
    }

    string get_text(HWND hwnd) {
        int len = ::GetWindowTextLengthW(hwnd);
        wstring w(len + 1, L'\0');
        ::GetWindowTextW(hwnd, w.data(), len + 1);
        w.resize(len);
        return narrow(w);
    }

    void set_text(HWND hwnd, const string& text) {
        ::SetWindowTextW(hwnd, wide(text).c_str());
    }

    void set_clipboard_text(HWND owner, const string& text) {
        wstring w = wide(text);
        if(!::OpenClipboard(owner)) return;
        ::EmptyClipboard();
        size_t bytes = (w.size() + 1) * sizeof(wchar_t);
        if(HGLOBAL h = ::GlobalAlloc(GMEM_MOVEABLE, bytes)) {
            memcpy(::GlobalLock(h), w.c_str(), bytes);
            ::GlobalUnlock(h);
            if(!::SetClipboardData(CF_UNICODETEXT, h)) ::GlobalFree(h);
        }
        ::CloseClipboard();
    }

    static string run_file_dialog(HWND owner, const wstring& title, FILEOPENDIALOGOPTIONS extra,
        const vector<pair<wstring, wstring>>& filters) {
        modal_scope scope;
        string result;
        IFileOpenDialog* dlg{nullptr};
        if(FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
            return result;
        FILEOPENDIALOGOPTIONS opts{};
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | extra);
        dlg->SetTitle(title.c_str());
        vector<COMDLG_FILTERSPEC> specs;
        for(auto& [name, spec] : filters) specs.push_back({name.c_str(), spec.c_str()});
        if(!specs.empty()) dlg->SetFileTypes(static_cast<UINT>(specs.size()), specs.data());
        if(SUCCEEDED(dlg->Show(owner))) {
            IShellItem* item{nullptr};
            if(SUCCEEDED(dlg->GetResult(&item))) {
                PWSTR path{nullptr};
                if(SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    result = narrow(path);
                    ::CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dlg->Release();
        return result;
    }

    string open_file_dialog(HWND owner, const wstring& title, const vector<pair<wstring, wstring>>& filters) {
        return run_file_dialog(owner, title, 0, filters);
    }

    string open_folder_dialog(HWND owner, const wstring& title) {
        return run_file_dialog(owner, title, FOS_PICKFOLDERS, {});
    }

    void message_box(HWND owner, const string& title, const string& text, UINT flags) {
        modal_scope scope;
        ::MessageBoxW(owner, wide(text).c_str(), wide(title).c_str(), flags);
    }

    void shell_open(const string& path, const string& args) {
        ::ShellExecuteW(nullptr, L"open", wide(path).c_str(), args.empty() ? nullptr : wide(args).c_str(),
            nullptr, SW_SHOWNORMAL);
    }

    static vector<HWND> dialogs;

    void register_dialog(HWND hwnd) { dialogs.push_back(hwnd); }

    void unregister_dialog(HWND hwnd) { erase(dialogs, hwnd); }

    bool route_dialog_message(MSG& msg) {
        for(HWND d : dialogs) {
            if(d == msg.hwnd || ::IsChild(d, msg.hwnd)) {
                // let multi-line edits keep Enter and Tab
                wchar_t cls[16]{};
                ::GetClassNameW(msg.hwnd, cls, 16);
                if(msg.message == WM_KEYDOWN && _wcsicmp(cls, L"Edit") == 0 &&
                   (::GetWindowLongW(msg.hwnd, GWL_STYLE) & ES_MULTILINE) &&
                   (msg.wParam == VK_RETURN || msg.wParam == VK_TAB))
                    return false;
                return ::IsDialogMessageW(d, &msg) != 0;
            }
        }
        return false;
    }

    static int modal_depth{0};
    static function<void()> modal_end;

    modal_scope::modal_scope() { modal_depth++; }

    modal_scope::~modal_scope() {
        // the host's handler posts a message, so deferred work runs after the code that opened the
        // dialog has returned and dropped its references
        if(--modal_depth == 0 && modal_end) modal_end();
    }

    bool in_modal() { return modal_depth > 0; }

    void on_modal_end(function<void()> fn) { modal_end = std::move(fn); }

    void run_modal(HWND hwnd, HWND owner) {
        modal_scope scope;
        if(owner) ::EnableWindow(owner, FALSE);
        MSG msg;
        while(::IsWindow(hwnd)) {
            const BOOL r = ::GetMessageW(&msg, nullptr, 0, 0);
            if(r <= 0) {
                // the app is quitting: close the dialog and hand WM_QUIT on to the outer loop
                if(r == 0) ::PostQuitMessage(static_cast<int>(msg.wParam));
                if(::IsWindow(hwnd)) ::DestroyWindow(hwnd);
                break;
            }
            if(route_dialog_message(msg)) continue;
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
        if(owner) {
            ::EnableWindow(owner, TRUE);
            ::SetForegroundWindow(owner);
        }
    }

    HWND child(HWND parent, const wchar_t* cls, const string& text, DWORD style, DWORD ex_style, int id, UINT dpi) {
        HWND h = ::CreateWindowExW(ex_style, cls, wide(text).c_str(), WS_CHILD | WS_VISIBLE | style,
            0, 0, 10, 10, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), ::GetModuleHandleW(nullptr), nullptr);
        ::SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(ui_font(dpi)), FALSE);
        return h;
    }

    void add_tooltip(HWND tooltip, HWND owner, HWND control, const string& text) {
        wstring w = wide(text);   // the tooltip copies the text
        TOOLINFOW ti{};
        ti.cbSize = sizeof(ti);
        ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        ti.hwnd = owner;
        ti.uId = reinterpret_cast<UINT_PTR>(control);
        ti.lpszText = w.data();
        ::SendMessageW(tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
    }
}
