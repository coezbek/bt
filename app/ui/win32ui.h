#pragma once
// Small Win32 helpers shared by the picker, toast and settings windows.
#include <windows.h>
#include <string>
#include <vector>
#include <functional>

namespace bt::ui {

    constexpr UINT WM_APP_REQUEST = WM_APP + 1;   // lParam: heap bt::request*, host owns it
    constexpr UINT WM_APP_TRAY = WM_APP + 2;
    constexpr UINT WM_APP_WARM = WM_APP + 3;
    constexpr UINT WM_APP_PARK = WM_APP + 4;

    std::wstring wide(const std::string& s);
    std::string narrow(const std::wstring& s);

    inline int scale(int v, UINT dpi) { return MulDiv(v, static_cast<int>(dpi), 96); }

    UINT dpi_of(HWND hwnd);
    UINT dpi_of_point(POINT pt);

    /**
     * @brief Work area of the monitor under the mouse cursor.
     */
    RECT cursor_work_area(UINT* dpi = nullptr);

    /**
     * @brief Message font (Segoe UI) at the given DPI, plus a size offset in points. Cached, never freed.
     */
    HFONT ui_font(UINT dpi, int size_delta_pt = 0, bool bold = false);
    HFONT mono_font(UINT dpi);
    HFONT icon_font(UINT dpi, int size_pt = 11);   // Segoe MDL2 Assets / Segoe Fluent Icons

    bool system_uses_dark_theme();
    void apply_dark_title_bar(HWND hwnd, bool dark);
    void apply_round_corners(HWND hwnd);

    std::string get_text(HWND hwnd);
    void set_text(HWND hwnd, const std::string& text);

    void set_clipboard_text(HWND owner, const std::string& text);

    /**
     * @brief Opens a file picker. filter is e.g. L"Executables\0*.exe\0". Returns empty when cancelled.
     */
    std::string open_file_dialog(HWND owner, const std::wstring& title, const std::vector<std::pair<std::wstring, std::wstring>>& filters);
    std::string open_folder_dialog(HWND owner, const std::wstring& title);

    void message_box(HWND owner, const std::string& title, const std::string& text, UINT flags = MB_OK | MB_ICONINFORMATION);

    /**
     * @brief Opens a path or URL with the shell (folders open in Explorer).
     */
    void shell_open(const std::string& path, const std::string& args = "");

    /**
     * @brief Dialog-like windows get keyboard navigation (Tab, Enter, Esc) through IsDialogMessage.
     */
    void register_dialog(HWND hwnd);
    void unregister_dialog(HWND hwnd);
    bool route_dialog_message(MSG& msg);

    /**
     * @brief Runs a nested message loop while hwnd lives, with owner disabled.
     */
    void run_modal(HWND hwnd, HWND owner);

    /**
     * @brief Creates a child control with the default UI font.
     */
    HWND child(HWND parent, const wchar_t* cls, const std::string& text, DWORD style, DWORD ex_style, int id, UINT dpi);

    void add_tooltip(HWND tooltip, HWND owner, HWND control, const std::string& text);
}
