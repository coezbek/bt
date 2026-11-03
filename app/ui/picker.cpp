#include "picker.h"
#include <format>
#include <map>
#include <algorithm>
#include <cmath>
#include <commctrl.h>
#include <uxtheme.h>
#include <dwmapi.h>
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>
#include "win32ui.h"
#include "icons.h"
#include "globals.h"
#include "../host.h"
#include "../request.h"
#include "common/url.h"

using namespace std;

namespace bt::ui {

    namespace {
        constexpr wchar_t CLASS_NAME[] = L"BrowserTamer.Picker";

        enum : int {
            ID_URL = 101, ID_TRACKERS, ID_RULE, ID_COPY, ID_EMAIL, ID_SETTINGS,
            ID_RULE_LOC = 110, ID_RULE_VALUE, ID_RULE_SCOPE, ID_RULE_REGEX, ID_RULE_APP
        };

        // Segoe MDL2 Assets / Segoe Fluent Icons code points
        constexpr wchar_t GLYPH_SHIELD[] = L"";
        constexpr wchar_t GLYPH_RULE[] = L"";
        constexpr wchar_t GLYPH_COPY[] = L"";
        constexpr wchar_t GLYPH_MAIL[] = L"";
        constexpr wchar_t GLYPH_SETTINGS[] = L"";
        constexpr wchar_t GLYPH_APP[] = L"";

        struct palette {
            COLORREF bg, text, dim, active, border, edit_bg, hint_bg, button_hover, toggle_on;
        };

        palette colours(bool dark) {
            if(dark) {
                return {RGB(32, 32, 32), RGB(240, 240, 240), RGB(160, 160, 160), RGB(62, 62, 62),
                        RGB(80, 80, 80), RGB(45, 45, 45), RGB(70, 70, 70), RGB(55, 55, 55), RGB(0, 95, 184)};
            }
            return {RGB(243, 243, 243), RGB(26, 26, 26), RGB(96, 96, 96), RGB(218, 218, 218),
                    RGB(200, 200, 200), RGB(255, 255, 255), RGB(225, 225, 225), RGB(229, 229, 229), RGB(153, 201, 255)};
        }

        HBRUSH cached_brush(COLORREF c) {
            static map<COLORREF, HBRUSH> brushes;
            auto it = brushes.find(c);
            if(it == brushes.end()) it = brushes.emplace(c, ::CreateSolidBrush(c)).first;
            return it->second;
        }

        int label_font_delta() {
            // label_size used to be an ImGui pixel offset from a 16 px base font; -4 matched the 9 pt UI font
            return static_cast<int>(lround((g_state.picker.label_size + 4.0f) * 0.75f));
        }

        int font_height(HFONT f) {
            HDC hdc = ::GetDC(nullptr);
            HGDIOBJ old = ::SelectObject(hdc, f);
            TEXTMETRICW tm{};
            ::GetTextMetricsW(hdc, &tm);
            ::SelectObject(hdc, old);
            ::ReleaseDC(nullptr, hdc);
            return tm.tmHeight;
        }

        int text_width(HFONT f, const wstring& s) {
            HDC hdc = ::GetDC(nullptr);
            HGDIOBJ old = ::SelectObject(hdc, f);
            SIZE sz{};
            ::GetTextExtentPoint32W(hdc, s.c_str(), static_cast<int>(s.size()), &sz);
            ::SelectObject(hdc, old);
            ::ReleaseDC(nullptr, hdc);
            return sz.cx;
        }

        int label_line_count() {
            switch(g_state.picker.label_display) {
                case label_display_mode::none: return 0;
                case label_display_mode::browser_and_profile: return 2;
                default: return 1;
            }
        }
    }

    bool picker::is_hotkey_down(uint32_t m) {
        const auto& inv = g_state.picker.invoke;
        bool shift = m & mod_shift, ctrl = m & mod_ctrl, alt = m & mod_alt, caps = m & mod_caps;
        return (inv.on_key_alt_shift && alt && shift) ||
               (inv.on_key_control_alt && ctrl && alt) ||
               (inv.on_key_control_shift && ctrl && shift) ||
               (inv.on_key_caps_locks && caps);
    }

    int picker::icon_size_for_dpi(UINT d) {
        int box = scale(static_cast<int>(g_state.picker.box_size), d);
        int label_h = font_height(ui_font(d, label_font_delta()));
        return max(scale(12, d), box - label_h * label_line_count());
    }

    void picker::create() {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = wndproc;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = ::LoadIconW(wc.hInstance, L"IDI_ICON1");
        wc.lpszClassName = CLASS_NAME;
        ::RegisterClassExW(&wc);

        wnd = ::CreateWindowExW(WS_EX_TOOLWINDOW, CLASS_NAME, wide(APP_LONG_NAME " - Pick").c_str(), WS_POPUP | WS_CLIPCHILDREN,
            0, 0, 100, 100, nullptr, nullptr, wc.hInstance, this);
        create_controls();
        park();
    }

    void picker::create_controls() {
        UINT d = dpi_of(wnd);
        url_edit = child(wnd, L"Edit", "", ES_AUTOHSCROLL | WS_TABSTOP, 0, ID_URL, d);
        ::SetWindowSubclass(url_edit, edit_proc, 1, reinterpret_cast<DWORD_PTR>(this));

        auto button = [&](int id) {
            return child(wnd, L"Button", "", BS_OWNERDRAW, 0, id, d);
        };
        btn_trackers = button(ID_TRACKERS);
        btn_rule = button(ID_RULE);
        btn_copy = button(ID_COPY);
        btn_email = button(ID_EMAIL);
        btn_settings = button(ID_SETTINGS);

        rule_loc = child(wnd, L"ComboBox", "", CBS_DROPDOWNLIST | WS_TABSTOP, 0, ID_RULE_LOC, d);
        for(const wchar_t* s : {L"URL", L"Window title", L"Process name", L"Lua function"})
            ::SendMessageW(rule_loc, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
        rule_value = child(wnd, L"Edit", "", ES_AUTOHSCROLL | WS_TABSTOP, 0, ID_RULE_VALUE, d);
        ::SetWindowSubclass(rule_value, edit_proc, 2, reinterpret_cast<DWORD_PTR>(this));
        rule_scope = child(wnd, L"ComboBox", "", CBS_DROPDOWNLIST | WS_TABSTOP, 0, ID_RULE_SCOPE, d);
        for(const wchar_t* s : {L"Anywhere", L"Host only", L"Path only"})
            ::SendMessageW(rule_scope, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
        rule_regex = button(ID_RULE_REGEX);
        rule_app = button(ID_RULE_APP);

        tooltip = ::CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
            0, 0, 0, 0, wnd, nullptr, ::GetModuleHandleW(nullptr), nullptr);
        add_tooltip(tooltip, wnd, url_edit, "Editable before opening");
        add_tooltip(tooltip, wnd, btn_trackers, "Trackers were removed from this link.\nClick to copy the original link.");
        add_tooltip(tooltip, wnd, btn_rule, "Create a rule for the browser you pick (R)");
        add_tooltip(tooltip, wnd, btn_copy, "Copy link and close");
        add_tooltip(tooltip, wnd, btn_email, "Email link and close");
        add_tooltip(tooltip, wnd, btn_settings, "Picker settings");
        add_tooltip(tooltip, wnd, rule_regex, "Value is a regular expression");
        add_tooltip(tooltip, wnd, rule_app, "Open as an app window (Chromium only)");
    }

    bool picker::visible() const {
        return wnd && !cloaked;
    }

    void picker::set_cloaked(bool on) {
        BOOL v = on ? TRUE : FALSE;
        ::DwmSetWindowAttribute(wnd, DWMWA_CLOAK, &v, sizeof(v));
        cloaked = on;
    }

    void picker::park() {
        // A cloaked window can still hold the focus. Hide it for real when it is active, so Windows
        // gives the focus back to the previous window, then put it back, cloaked and off-screen.
        if(::GetForegroundWindow() == wnd) ::ShowWindow(wnd, SW_HIDE);
        set_cloaked(true);
        ::SetWindowPos(wnd, HWND_BOTTOM, -32000, -32000, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        mouse_down = -1;
    }

    bool picker::apply_style() {
        const auto& ps = g_state.picker;

        LONG_PTR style = WS_POPUP | WS_CLIPCHILDREN;
        if(ps.show_native_chrome) style |= WS_CAPTION | WS_SYSMENU;
        // WS_EX_TOOLWINDOW: the parked window is technically visible and must not get a taskbar button
        LONG_PTR ex = WS_EX_TOOLWINDOW;
        if(ps.always_on_top) ex |= WS_EX_TOPMOST;
        if(ps.opacity < 0.999f) ex |= WS_EX_LAYERED;
        const auto alpha = static_cast<BYTE>(clamp(ps.opacity, 0.1f, 1.0f) * 255);

        bool frame_changed{false};
        if(style != applied.style) {
            ::SetWindowLongPtrW(wnd, GWL_STYLE, style);
            applied.style = style;
            frame_changed = true;
        }
        if(ex != applied.ex_style) {
            ::SetWindowLongPtrW(wnd, GWL_EXSTYLE, ex);
            applied.ex_style = ex;
            applied.alpha = 0;
            frame_changed = true;
        }
        if((ex & WS_EX_LAYERED) && alpha != applied.alpha) {
            ::SetLayeredWindowAttributes(wnd, 0, alpha, LWA_ALPHA);
            applied.alpha = alpha;
        }
        if(static_cast<int>(dark) != applied.dark || frame_changed) {
            apply_dark_title_bar(wnd, dark);
            apply_round_corners(wnd);
            const wchar_t* theme = dark ? L"DarkMode_CFD" : nullptr;
            ::SetWindowTheme(rule_loc, theme, nullptr);
            ::SetWindowTheme(rule_scope, theme, nullptr);
            ::SetWindowTheme(url_edit, dark ? L"DarkMode_Explorer" : nullptr, nullptr);
            applied.dark = dark;
        }
        return frame_changed;
    }

    void picker::compute_layout() {
        const auto& ps = g_state.picker;
        RECT work = cursor_work_area(&dpi);
        const int work_w = work.right - work.left, work_h = work.bottom - work.top;

        pad = scale(8, dpi);
        btn_w = scale(30, dpi);
        header_h = scale(30, dpi);
        rule_h = rule_open ? scale(30, dpi) + pad / 2 : 0;

        box = scale(static_cast<int>(ps.box_size), dpi);
        margin = scale(static_cast<int>(ps.item_padding), dpi);
        tile = box + margin * 2;

        label_lines = label_line_count();
        label_h = font_height(ui_font(dpi, label_font_delta()));
        icon_px = icon_size_for_dpi(dpi);

        const int n = max(1, static_cast<int>(choices.size()));
        const int buttons = (cp.trackers_removed > 0 ? 5 : 4);
        const int url_w = text_width(ui_font(dpi), wide(cp.url)) + scale(16, dpi) + btn_w * (buttons + 2);
        const int grid_w = tile * n + pad * 2;
        const int min_w = work_w * ps.min_width_perc / 100;
        const int max_w = max(min_w, work_w * ps.max_width_perc / 100);
        const int w = clamp(max(url_w, grid_w), min_w, max_w);

        per_row = max(1, (w - pad * 2) / tile);
        rows = (n + per_row - 1) / per_row;
        grid_y = pad + header_h + rule_h + pad / 2;
        int h = grid_y + tile * rows + pad;
        h = min(h, work_h);
        client = {w, h};

        // centre the used part of the grid
        const int used = min(n, per_row) * tile;
        grid_x = (w - used) / 2;

        RECT rc{0, 0, w, h};
        ::AdjustWindowRectExForDpi(&rc, static_cast<DWORD>(::GetWindowLongPtrW(wnd, GWL_STYLE)), FALSE,
            static_cast<DWORD>(::GetWindowLongPtrW(wnd, GWL_EXSTYLE)), dpi);
        const int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
        const int x = work.left + (work_w - ww) / 2;
        const int y = work.top + (work_h - wh) / 2;
        target = {x, y, x + ww, y + wh};
    }

    void picker::move_window(UINT extra_flags) {
        ::SetWindowPos(wnd, g_state.picker.always_on_top ? HWND_TOPMOST : HWND_NOTOPMOST,
            target.left, target.top, target.right - target.left, target.bottom - target.top, extra_flags);
    }

    void picker::place_controls() {
        HFONT font = ui_font(dpi);
        if(font_dpi != dpi) {
            for(HWND h : {url_edit, rule_loc, rule_value, rule_scope}) ::SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
            font_dpi = dpi;
        }

        const int bh = header_h;
        int right = client.cx - pad;
        auto place_btn = [&](HWND h, bool show) {
            ::ShowWindow(h, show ? SW_SHOWNA : SW_HIDE);
            if(!show) return;
            right -= btn_w;
            ::MoveWindow(h, right, pad, btn_w, bh, FALSE);
        };
        place_btn(btn_settings, true);
        place_btn(btn_email, true);
        place_btn(btn_copy, true);
        place_btn(btn_rule, true);
        right -= scale(4, dpi);

        int left = pad;
        ::ShowWindow(btn_trackers, cp.trackers_removed > 0 ? SW_SHOWNA : SW_HIDE);
        if(cp.trackers_removed > 0) {
            ::MoveWindow(btn_trackers, left, pad, btn_w, bh, FALSE);
            left += btn_w + scale(4, dpi);
        }

        const int edit_h = font_height(font) + scale(8, dpi);
        ::MoveWindow(url_edit, left, pad + (bh - edit_h) / 2, max(10, right - left), edit_h, FALSE);

        // rule creator row
        const int show = rule_open ? SW_SHOWNA : SW_HIDE;
        for(HWND h : {rule_loc, rule_value, rule_scope, rule_regex, rule_app}) ::ShowWindow(h, show);
        if(rule_open) {
            const int y = pad + header_h + pad / 2;
            const int row_h = scale(30, dpi);
            const int loc_w = scale(120, dpi), scope_w = scale(100, dpi);
            int x = pad;
            ::MoveWindow(rule_loc, x, y + (row_h - edit_h) / 2, loc_w, scale(200, dpi), FALSE);
            x += loc_w + scale(4, dpi);
            int value_w = client.cx - pad - x - scope_w - btn_w * 2 - scale(12, dpi);
            ::MoveWindow(rule_value, x, y + (row_h - edit_h) / 2, max(10, value_w), edit_h, FALSE);
            x += value_w + scale(4, dpi);
            ::MoveWindow(rule_scope, x, y + (row_h - edit_h) / 2, scope_w, scale(200, dpi), FALSE);
            x += scope_w + scale(4, dpi);
            ::MoveWindow(rule_regex, x, y, btn_w, row_h, FALSE);
            x += btn_w + scale(4, dpi);
            ::MoveWindow(rule_app, x, y, btn_w, row_h, FALSE);
        }
    }

    void picker::show(const click_payload& payload, vector<profile_selection> list, done_fn on_done) {
        if(visible()) finish(nullopt);

        cp = payload;
        choices = std::move(list);
        done = std::move(on_done);
        active = 0;
        rule_open = false;
        rule_regex_on = rule_app_on = false;
        closing = false;
        dark = system_uses_dark_theme();
        const bool frame_changed = apply_style();
        compute_layout();
        set_text(url_edit, cp.url);

        // the rule creator starts with the link's host as a URL host rule
        string proto, host, path;
        match_rule::parse_url(cp.url, proto, host, path);
        set_text(rule_value, host);
        ::SendMessageW(rule_loc, CB_SETCURSEL, 0, 0);
        ::SendMessageW(rule_scope, CB_SETCURSEL, host.empty() ? 0 : 1, 0);

        place_controls();
        render();
        // The window never hides; it waits off-screen and cloaked. Showing a hidden window costs
        // 20-40 ms in the window manager, moving and uncloaking a visible one costs under 1 ms.
        move_window(SWP_NOACTIVATE | SWP_SHOWWINDOW | (frame_changed ? SWP_FRAMECHANGED : 0));
        set_cloaked(false);
        ::SetForegroundWindow(wnd);
        ::SetFocus(wnd);
        ::UpdateWindow(wnd);
    }

    void picker::warm() {
        if(visible()) return;
        dark = system_uses_dark_theme();
        apply_style();
        cp = click_payload{};
        cp.url = "https://example.com/";
        choices.clear();
        for(const browser& b : g_state.browsers) {
            if(b.is_hidden) continue;
            for(size_t i = 0; i < b.profiles.size(); i++) {
                if(!b.profiles[i].is_hidden) choices.emplace_back(b, i);
            }
        }
        compute_layout();
        set_text(url_edit, cp.url);
        place_controls();
        render();
        choices.clear();
    }

    void picker::cancel() {
        if(visible()) finish(nullopt);
    }

    RECT picker::tile_rect(int i) const {
        int row = i / per_row, col = i % per_row;
        int row_count = min(per_row, static_cast<int>(choices.size()) - row * per_row);
        int x0 = (client.cx - row_count * tile) / 2;
        int x = x0 + col * tile, y = grid_y + row * tile;
        return {x, y, x + tile, y + tile};
    }

    int picker::hit_test(POINT pt) const {
        for(int i = 0; i < static_cast<int>(choices.size()); i++) {
            RECT r = tile_rect(i);
            if(::PtInRect(&r, pt)) return i;
        }
        return -1;
    }

    void picker::set_active(int i) {
        if(choices.empty()) return;
        const int n = static_cast<int>(choices.size());
        i = ((i % n) + n) % n;
        if(i == active) return;
        RECT r1 = tile_rect(active), r2 = tile_rect(i);
        active = i;
        render();
        ::InvalidateRect(wnd, &r1, FALSE);
        ::InvalidateRect(wnd, &r2, FALSE);
    }

    void picker::toggle_rule_creator() {
        rule_open = !rule_open;
        compute_layout();
        place_controls();
        render();
        move_window(SWP_NOACTIVATE);
        ::InvalidateRect(wnd, nullptr, FALSE);
        if(rule_open) ::SetFocus(rule_value);
        else ::SetFocus(wnd);
    }

    optional<match_rule> picker::build_rule() const {
        if(!rule_open) return nullopt;
        match_rule r;
        r.value = get_text(rule_value);
        if(r.value.empty()) return nullopt;
        r.loc = static_cast<match_location>(max<LRESULT>(0, ::SendMessageW(rule_loc, CB_GETCURSEL, 0, 0)));
        r.scope = static_cast<match_scope>(max<LRESULT>(0, ::SendMessageW(rule_scope, CB_GETCURSEL, 0, 0)));
        r.is_regex = rule_regex_on;
        r.app_mode = rule_app_on;
        return r;
    }

    void picker::choose(int i) {
        if(i < 0 || i >= static_cast<int>(choices.size())) return;
        active = i;
        finish(i);
    }

    void picker::finish(optional<int> idx) {
        if(closing) return;
        closing = true;
        cp.url = get_text(url_edit);
        optional<match_rule> rule = idx ? build_rule() : nullopt;
        optional<profile_selection> choice;
        if(idx) choice = choices[*idx];

        // disappear at once; the slower focus hand-back runs after the browser has been started
        set_cloaked(true);
        done_fn d = std::move(done);
        done = nullptr;
        if(d) d(choice, cp, rule);
        choices.clear();
        ::PostMessageW(wnd, WM_APP_PARK, 0, 0);
    }

    void picker::on_command(int id, int code) {
        switch(id) {
            case ID_TRACKERS:
                set_clipboard_text(wnd, cp.raw_url);
                break;
            case ID_RULE:
                toggle_rule_creator();
                break;
            case ID_COPY:
                set_clipboard_text(wnd, get_text(url_edit));
                finish(nullopt);
                break;
            case ID_EMAIL: {
                string link = get_text(url_edit);
                set_clipboard_text(wnd, link);
                finish(nullopt);
                shell_open(format("mailto:?body={}", link));
                break;
            }
            case ID_SETTINGS:
                finish(nullopt);
                host::show_settings(1);
                break;
            case ID_RULE_REGEX:
                rule_regex_on = !rule_regex_on;
                ::InvalidateRect(rule_regex, nullptr, FALSE);
                break;
            case ID_RULE_APP:
                rule_app_on = !rule_app_on;
                ::InvalidateRect(rule_app, nullptr, FALSE);
                break;
            case ID_RULE_LOC:
                if(code == CBN_SELCHANGE) {
                    bool is_url = ::SendMessageW(rule_loc, CB_GETCURSEL, 0, 0) == 0;
                    ::EnableWindow(rule_scope, is_url);
                }
                break;
            default:
                break;
        }
    }

    void picker::draw_button(const DRAWITEMSTRUCT& di) {
        const palette p = colours(dark);
        HDC hdc = di.hDC;
        RECT rc = di.rcItem;
        bool pressed = di.itemState & ODS_SELECTED;
        bool on = (di.CtlID == ID_RULE && rule_open) || (di.CtlID == ID_RULE_REGEX && rule_regex_on) ||
                  (di.CtlID == ID_RULE_APP && rule_app_on);
        COLORREF bg = on ? p.toggle_on : (pressed ? p.active : p.bg);
        ::FillRect(hdc, &rc, cached_brush(bg));

        const wchar_t* glyph{nullptr};
        HFONT font = icon_font(dpi);
        COLORREF fg = p.text;
        switch(di.CtlID) {
            case ID_TRACKERS: glyph = GLYPH_SHIELD; fg = RGB(220, 80, 60); break;
            case ID_RULE: glyph = GLYPH_RULE; break;
            case ID_COPY: glyph = GLYPH_COPY; break;
            case ID_EMAIL: glyph = GLYPH_MAIL; break;
            case ID_SETTINGS: glyph = GLYPH_SETTINGS; break;
            case ID_RULE_APP: glyph = GLYPH_APP; break;
            case ID_RULE_REGEX: glyph = L".*"; font = ui_font(dpi, 1, true); break;
            default: break;
        }
        if(!glyph) return;
        ::SetBkMode(hdc, TRANSPARENT);
        ::SetTextColor(hdc, fg);
        HGDIOBJ old = ::SelectObject(hdc, font);
        ::DrawTextW(hdc, glyph, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        ::SelectObject(hdc, old);
    }

    void picker::paint_tiles(HDC hdc) {
        const palette pal = colours(dark);
        const auto& ps = g_state.picker;
        const int n = static_cast<int>(choices.size());

        // shapes with anti-aliasing first, then icons and text with GDI
        {
            Gdiplus::Graphics g{hdc};
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            auto to_color = [](COLORREF c) { return Gdiplus::Color{255, GetRValue(c), GetGValue(c), GetBValue(c)}; };

            if(active >= 0 && active < n) {
                RECT r = tile_rect(active);
                float rad = static_cast<float>(scale(static_cast<int>(ps.item_rounding), dpi));
                rad = min(rad, tile / 2.0f);
                Gdiplus::GraphicsPath path;
                float x = static_cast<float>(r.left), y = static_cast<float>(r.top), w = static_cast<float>(tile), h = static_cast<float>(tile);
                if(rad > 0.5f) {
                    float d = rad * 2;
                    path.AddArc(x, y, d, d, 180, 90);
                    path.AddArc(x + w - d, y, d, d, 270, 90);
                    path.AddArc(x + w - d, y + h - d, d, d, 0, 90);
                    path.AddArc(x, y + h - d, d, d, 90, 90);
                    path.CloseFigure();
                } else {
                    path.AddRectangle(Gdiplus::RectF{x, y, w, h});
                }
                Gdiplus::SolidBrush brush{to_color(pal.active)};
                g.FillPath(&brush, &path);
            }
        }

        HFONT label_font = ui_font(dpi, label_font_delta());
        HGDIOBJ old_font = ::SelectObject(hdc, label_font);
        ::SetBkMode(hdc, TRANSPARENT);

        for(int i = 0; i < n; i++) {
            const RECT r = tile_rect(i);
            const profile_selection& c = choices[i];

            HBITMAP bmp = icons::profile_bitmap(c.b(), c.p(), icon_px, g_state.icon_overlay);
            icons::draw(hdc, bmp, r.left + (tile - icon_px) / 2, r.top + margin);

            if(ps.show_key_hints && i < 10) {
                wstring key = to_wstring((i + 1) % 10);
                int d = label_h;
                int cx = r.left + tile / 2, cy = r.top + margin;
                RECT hint{cx - d / 2, cy - d / 2, cx - d / 2 + d, cy - d / 2 + d};
                HBRUSH old_b = static_cast<HBRUSH>(::SelectObject(hdc, cached_brush(pal.hint_bg)));
                HGDIOBJ old_p = ::SelectObject(hdc, ::GetStockObject(NULL_PEN));
                ::Ellipse(hdc, hint.left, hint.top, hint.right + 1, hint.bottom + 1);
                ::SelectObject(hdc, old_p);
                ::SelectObject(hdc, old_b);
                ::SetTextColor(hdc, pal.text);
                ::DrawTextW(hdc, key.c_str(), -1, &hint, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            }

            if(label_lines > 0) {
                string line1, line2;
                switch(ps.label_display) {
                    case label_display_mode::browser_and_profile:
                        line1 = c.b().name;
                        line2 = c.p().name;
                        break;
                    case label_display_mode::browser:
                        line1 = c.b().name;
                        break;
                    case label_display_mode::profile:
                        line1 = c.p().name;
                        break;
                    default:
                        break;
                }
                // a browser without profiles repeats its name; show it once
                if(line2 == line1) line2.clear();

                int top = r.top + margin + icon_px + scale(2, dpi);
                RECT l1{r.left + margin / 4, top, r.right - margin / 4, top + label_h};
                RECT l2{l1.left, l1.bottom, l1.right, l1.bottom + label_h};
                const UINT flags = DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX;
                ::SetTextColor(hdc, pal.text);
                wstring w1 = wide(line1), w2 = wide(line2);
                ::DrawTextW(hdc, w1.c_str(), -1, &l1, flags);
                ::SetTextColor(hdc, pal.dim);
                ::DrawTextW(hdc, w2.c_str(), -1, &l2, flags);
            }
        }
        ::SelectObject(hdc, old_font);

        if(n == 0) {
            RECT rc{0, grid_y, client.cx, client.cy};
            ::SetTextColor(hdc, RGB(220, 80, 60));
            ::DrawTextW(hdc, L"no browsers", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }

    void picker::render() {
        const int w = client.cx, h = client.cy;
        if(w <= 0 || h <= 0) return;

        if(!back_dc || back_size.cx != w || back_size.cy != h) {
            if(back_dc) {
                ::SelectObject(back_dc, back_old);
                ::DeleteObject(back_bmp);
                ::DeleteDC(back_dc);
            }
            HDC screen = ::GetDC(nullptr);
            back_dc = ::CreateCompatibleDC(screen);
            back_bmp = ::CreateCompatibleBitmap(screen, w, h);
            ::ReleaseDC(nullptr, screen);
            back_old = ::SelectObject(back_dc, back_bmp);
            back_size = {w, h};
        }

        HDC hdc = back_dc;
        RECT rc{0, 0, w, h};
        const palette pal = colours(dark);
        ::FillRect(hdc, &rc, cached_brush(pal.bg));
        paint_tiles(hdc);

        int bw = g_state.picker.border_width;
        if(bw > 0 && !g_state.picker.show_native_chrome) {
            HBRUSH b = cached_brush(pal.border);
            bw = scale(bw, dpi);
            for(RECT e : {RECT{0, 0, w, bw}, RECT{0, h - bw, w, h}, RECT{0, 0, bw, h}, RECT{w - bw, 0, w, h}})
                ::FillRect(hdc, &e, b);
        }
    }

    void picker::paint(HDC target_dc) {
        if(back_dc) ::BitBlt(target_dc, 0, 0, back_size.cx, back_size.cy, back_dc, 0, 0, SRCCOPY);
    }

    LRESULT CALLBACK picker::edit_proc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR ref) {
        auto* self = reinterpret_cast<picker*>(ref);
        if(m == WM_KEYDOWN) {
            if(w == VK_RETURN) {
                self->choose(self->active);
                return 0;
            }
            if(w == VK_ESCAPE) {
                ::SetFocus(self->wnd);
                return 0;
            }
        }
        if(m == WM_CHAR && (w == VK_RETURN || w == VK_ESCAPE)) return 0;   // no beep
        return ::DefSubclassProc(h, m, w, l);
    }

    LRESULT CALLBACK picker::wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
        picker* self{nullptr};
        if(m == WM_NCCREATE) {
            self = static_cast<picker*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
            self->wnd = h;
            ::SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<picker*>(::GetWindowLongPtrW(h, GWLP_USERDATA));
        }
        return self ? self->handle(m, w, l) : ::DefWindowProcW(h, m, w, l);
    }

    LRESULT picker::handle(UINT m, WPARAM w, LPARAM l) {
        switch(m) {
            case WM_ERASEBKGND:
                return 1;
            case WM_PAINT: {
                PAINTSTRUCT ps;
                HDC hdc = ::BeginPaint(wnd, &ps);
                paint(hdc);
                ::EndPaint(wnd, &ps);
                return 0;
            }
            case WM_DRAWITEM:
                draw_button(*reinterpret_cast<DRAWITEMSTRUCT*>(l));
                return TRUE;
            case WM_CTLCOLOREDIT: {
                const palette p = colours(dark);
                HDC hdc = reinterpret_cast<HDC>(w);
                ::SetTextColor(hdc, p.text);
                ::SetBkColor(hdc, p.edit_bg);
                return reinterpret_cast<LRESULT>(cached_brush(p.edit_bg));
            }
            case WM_COMMAND:
                on_command(LOWORD(w), HIWORD(w));
                return 0;
            case WM_ACTIVATE:
                if(LOWORD(w) == WA_INACTIVE && g_state.picker.close_on_focus_loss && visible() && !closing) {
                    // a combo box drop-down belongs to us; anything else means the user went elsewhere
                    HWND to = reinterpret_cast<HWND>(l);
                    DWORD pid{0};
                    if(to) ::GetWindowThreadProcessId(to, &pid);
                    if(pid != ::GetCurrentProcessId()) finish(nullopt);
                }
                return 0;
            case WM_CLOSE:
                finish(nullopt);
                return 0;
            case WM_APP_PARK:
                if(cloaked) park();   // a new show() may have happened in between
                return 0;
            case WM_DPICHANGED:
                // layout is computed for the target monitor before the move; ignore the suggested rect
                return 0;
            case WM_MOUSEMOVE: {
                POINT pt{static_cast<short>(LOWORD(l)), static_cast<short>(HIWORD(l))};
                int i = hit_test(pt);
                if(i >= 0) set_active(i);
                return 0;
            }
            case WM_LBUTTONDOWN: {
                POINT pt{static_cast<short>(LOWORD(l)), static_cast<short>(HIWORD(l))};
                mouse_down = hit_test(pt);
                return 0;
            }
            case WM_LBUTTONUP: {
                // only a full click on a tile counts; a release left over from the click that
                // opened the link must not pick whatever tile appeared under the cursor
                POINT pt{static_cast<short>(LOWORD(l)), static_cast<short>(HIWORD(l))};
                int i = hit_test(pt);
                if(i >= 0 && i == mouse_down) choose(i);
                mouse_down = -1;
                return 0;
            }
            case WM_SETCURSOR:
                if(LOWORD(l) == HTCLIENT) {
                    POINT pt;
                    ::GetCursorPos(&pt);
                    ::ScreenToClient(wnd, &pt);
                    if(hit_test(pt) >= 0) {
                        ::SetCursor(::LoadCursorW(nullptr, IDC_HAND));
                        return TRUE;
                    }
                }
                break;
            case WM_KEYDOWN: {
                const int n = static_cast<int>(choices.size());
                switch(w) {
                    case VK_ESCAPE:
                        finish(nullopt);
                        return 0;
                    case VK_RETURN:
                        choose(active);
                        return 0;
                    case VK_LEFT:
                    case VK_UP:
                        set_active(active - 1);
                        return 0;
                    case VK_RIGHT:
                    case VK_DOWN:
                    case VK_TAB:
                        set_active(active + 1);
                        return 0;
                    case 'R':
                        toggle_rule_creator();
                        return 0;
                    default:
                        break;
                }
                int digit = -1;
                if(w >= '0' && w <= '9') digit = static_cast<int>(w - '0');
                if(w >= VK_NUMPAD0 && w <= VK_NUMPAD9) digit = static_cast<int>(w - VK_NUMPAD0);
                if(digit >= 0) {
                    int idx = digit == 0 ? 9 : digit - 1;
                    if(idx < n) choose(idx);
                    return 0;
                }
                break;
            }
            default:
                break;
        }
        return ::DefWindowProcW(wnd, m, w, l);
    }
}
