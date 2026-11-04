#include "picker.h"
#include <format>
#include <string_view>
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
            ID_REMEMBER = 110, ID_REMEMBER_KIND, ID_REMEMBER_VALUE
        };

        // Segoe MDL2 Assets / Segoe Fluent Icons code points
        constexpr wchar_t GLYPH_SHIELD[] = L"";
        constexpr wchar_t GLYPH_RULE[] = L"";
        constexpr wchar_t GLYPH_COPY[] = L"";
        constexpr wchar_t GLYPH_MAIL[] = L"";
        constexpr wchar_t GLYPH_SETTINGS[] = L"";
        constexpr wchar_t GLYPH_CHECK[] = L"\uE73E";
        constexpr UINT_PTR SHIFT_TIMER = 1;
        constexpr UINT WM_APP_FOCUS_SELF = WM_APP + 30;

        // what the remember section can match; Host is the default
        enum remember_kind : int { kind_host = 0, kind_regex, kind_url, kind_title, kind_process };

        string regex_escape(const string& s) {
            string r;
            for(char c : s) {
                if(string_view{".^$|()[]{}*+?\\"}.find(c) != string_view::npos) r += '\\';
                r += c;
            }
            return r;
        }

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

        rule_check = button(ID_REMEMBER);
        rule_kind = child(wnd, L"ComboBox", "", CBS_DROPDOWNLIST | WS_TABSTOP, 0, ID_REMEMBER_KIND, d);
        for(const wchar_t* k : {L"Host", L"Regex", L"URL contains", L"Window title", L"Process"})
            ::SendMessageW(rule_kind, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(k));
        rule_value = child(wnd, L"Edit", "", ES_AUTOHSCROLL | WS_TABSTOP, 0, ID_REMEMBER_VALUE, d);
        ::SetWindowSubclass(rule_value, edit_proc, 2, reinterpret_cast<DWORD_PTR>(this));

        tooltip = ::CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
            0, 0, 0, 0, wnd, nullptr, ::GetModuleHandleW(nullptr), nullptr);
        add_tooltip(tooltip, wnd, url_edit, "Editable before opening");
        add_tooltip(tooltip, wnd, btn_trackers, "Trackers were removed from this link.\nClick to copy the original link.");
        add_tooltip(tooltip, wnd, btn_rule, "Remember the choice: add a rule for the profile you pick (R, or hold Shift)");
        add_tooltip(tooltip, wnd, btn_copy, "Copy link and close");
        add_tooltip(tooltip, wnd, btn_email, "Email link and close");
        add_tooltip(tooltip, wnd, btn_settings, "Settings");
        add_tooltip(tooltip, wnd, rule_check, "Add a rule for the profile you pick. Follows Shift until you click it.");
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

        // keep WS_VISIBLE: the parked window must stay visible, or the next show takes the slow path
        LONG_PTR style = WS_POPUP | WS_CLIPCHILDREN | (::GetWindowLongPtrW(wnd, GWL_STYLE) & WS_VISIBLE);
        if(ps.show_native_chrome) style |= WS_CAPTION | WS_SYSMENU;
        // WS_EX_TOOLWINDOW: the parked window is technically visible and must not get a taskbar button
        LONG_PTR ex = WS_EX_TOOLWINDOW;
        if(ps.always_on_top) ex |= WS_EX_TOPMOST;
        if(ps.opacity < 0.999f) ex |= WS_EX_LAYERED;
        const auto alpha = static_cast<BYTE>(clamp(ps.opacity, 0.1f, 1.0f) * 255);

        bool frame_changed{false};
        if((style & ~WS_VISIBLE) != (applied.style & ~WS_VISIBLE)) {
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
            ::SetWindowTheme(rule_kind, theme, nullptr);
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
        footer_h = section_open ? scale(30, dpi) : font_height(ui_font(dpi, -1));

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
        grid_y = pad + header_h + pad / 2;
        footer_y = grid_y + tile * rows + pad / 2;
        int h = footer_y + footer_h + pad;
        h = min(h, work_h);
        client = {w, h};

        // centre the used part of the grid
        const int used = min(n, per_row) * tile;
        grid_x = (w - used) / 2;

        RECT rc{0, 0, w, h};
        ::AdjustWindowRectExForDpi(&rc, static_cast<DWORD>(::GetWindowLongPtrW(wnd, GWL_STYLE)), FALSE,
            static_cast<DWORD>(::GetWindowLongPtrW(wnd, GWL_EXSTYLE)), dpi);
        const int ww = rc.right - rc.left, wh = rc.bottom - rc.top;

        // Open at the click: the first tile centred on the mouse, shifted only as far as needed to stay
        // on screen. rc.left/top are the (negative) frame offsets.
        POINT pt{};
        ::GetCursorPos(&pt);
        const int first_tile_cx = (w - min(n, per_row) * tile) / 2 + tile / 2;
        const int first_tile_cy = grid_y + tile / 2;
        int x = pt.x - first_tile_cx + rc.left;
        int y = pt.y - first_tile_cy + rc.top;
        x = clamp(x, static_cast<int>(work.left), max(static_cast<int>(work.left), static_cast<int>(work.right) - ww));
        y = clamp(y, static_cast<int>(work.top), max(static_cast<int>(work.top), static_cast<int>(work.bottom) - wh));
        target = {x, y, x + ww, y + wh};
    }

    void picker::move_window(UINT extra_flags) {
        ::SetWindowPos(wnd, g_state.picker.always_on_top ? HWND_TOPMOST : HWND_NOTOPMOST,
            target.left, target.top, target.right - target.left, target.bottom - target.top, extra_flags);
    }

    void picker::place_controls() {
        HFONT font = ui_font(dpi);
        if(font_dpi != dpi) {
            for(HWND h : {url_edit, rule_kind, rule_value}) ::SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
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

        // A single-line Edit draws its text at the top. So the edit is exactly one line high and centred
        // in a taller box that render() paints, which centres the text vertically.
        const int text_h = font_height(font);
        const int edit_h = text_h + scale(8, dpi);
        const int inset = scale(6, dpi);
        auto place_field = [&](HWND h, RECT& box, int x, int y, int w) {
            box = {x, y, x + w, y + edit_h};
            ::MoveWindow(h, x + inset, y + (edit_h - text_h) / 2, max(10, w - inset * 2), text_h, FALSE);
        };
        place_field(url_edit, url_box, left, pad + (bh - edit_h) / 2, max(10, right - left));
        rule_box = {};

        // remember section, below the tiles so they never move away from the mouse
        const int show = section_open ? SW_SHOWNA : SW_HIDE;
        for(HWND h : {rule_check, rule_kind, rule_value}) ::ShowWindow(h, show);
        if(section_open) {
            const int y = footer_y, row_h = footer_h;
            const int check_w = scale(110, dpi), kind_w = scale(120, dpi);
            int x = pad;
            ::MoveWindow(rule_check, x, y, check_w, row_h, FALSE);
            x += check_w + scale(4, dpi);
            ::MoveWindow(rule_kind, x, y + (row_h - edit_h) / 2, kind_w, scale(200, dpi), FALSE);
            x += kind_w + scale(4, dpi);
            place_field(rule_value, rule_box, x, y + (row_h - edit_h) / 2, max(10, static_cast<int>(client.cx) - pad - x));
        }
    }

    void picker::show(const click_payload& payload, vector<profile_selection> list, done_fn on_done) {
        if(visible()) finish(nullopt);

        cp = payload;
        choices = std::move(list);
        done = std::move(on_done);
        active = 0;
        closing = false;
        // the section follows Shift until the user changes the checkbox by hand
        shift_down = (::GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        remember = section_open = shift_down;
        remember_manual = false;
        value_edited = false;
        dark = system_uses_dark_theme();
        const bool frame_changed = apply_style();
        compute_layout();
        set_text(url_edit, cp.url);

        ::SendMessageW(rule_kind, CB_SETCURSEL, kind_host, 0);
        fill_rule_value();

        place_controls();
        render();
        // The window never hides; it waits off-screen and cloaked. Showing a hidden window costs
        // 20-40 ms in the window manager, moving and uncloaking a visible one costs under 1 ms.
        move_window(SWP_NOACTIVATE | SWP_SHOWWINDOW | (frame_changed ? SWP_FRAMECHANGED : 0));
        set_cloaked(false);
        ::SetForegroundWindow(wnd);
        ::SetFocus(wnd);
        ::UpdateWindow(wnd);
        ::SetTimer(wnd, SHIFT_TIMER, 30, nullptr);
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

    void picker::relayout_in_place() {
        RECT before{};
        ::GetWindowRect(wnd, &before);
        compute_layout();
        // keep the window where it is; it only grows or shrinks at the bottom, and moves up only
        // as far as needed to stay on screen
        const int h = target.bottom - target.top;
        HMONITOR mon = ::MonitorFromWindow(wnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{sizeof(mi)};
        ::GetMonitorInfoW(mon, &mi);
        const int top = max(static_cast<int>(mi.rcWork.top), min(static_cast<int>(before.top), static_cast<int>(mi.rcWork.bottom) - h));
        target = {before.left, top, before.left + (target.right - target.left), top + h};
        place_controls();
        render();
        move_window(SWP_NOACTIVATE);
        ::InvalidateRect(wnd, nullptr, FALSE);
    }

    void picker::set_remember(bool on, bool manual) {
        if(manual) remember_manual = true;
        remember = on;
        // once shown, the row stays; releasing Shift only unchecks the box
        if(on && !section_open) {
            section_open = true;
            relayout_in_place();
        }
        for(HWND h : {rule_check, btn_rule}) ::InvalidateRect(h, nullptr, FALSE);
    }

    void picker::fill_rule_value() {
        string proto, host, path;
        match_rule::parse_url(cp.url, proto, host, path);
        string v;
        switch(static_cast<int>(::SendMessageW(rule_kind, CB_GETCURSEL, 0, 0))) {
            case kind_host: v = host; break;
            case kind_regex: v = host.empty() ? "" : "https?://" + regex_escape(host) + "([/?#].*)?"; break;
            case kind_url: v = cp.url; break;
            case kind_title: v = cp.window_title; break;
            case kind_process: v = cp.process_name; break;
            default: break;
        }
        filling_value = true;
        set_text(rule_value, v);
        filling_value = false;
    }

    void picker::poll_shift() {
        // typing capitals into the URL or value field, or using the combo box, must not toggle the box
        HWND focus = ::GetFocus();
        if(remember_manual || (focus && focus != wnd)) return;
        bool down = (::GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        if(down == shift_down) return;
        shift_down = down;
        set_remember(down, false);
    }

    optional<match_rule> picker::build_rule() const {
        if(!remember) return nullopt;
        match_rule r;
        r.value = get_text(rule_value);
        if(r.value.empty()) return nullopt;
        switch(static_cast<int>(::SendMessageW(rule_kind, CB_GETCURSEL, 0, 0))) {
            case kind_host: r.loc = match_location::url; r.scope = match_scope::domain; break;
            case kind_regex: r.loc = match_location::url; r.is_regex = true; break;
            case kind_url: r.loc = match_location::url; break;
            case kind_title: r.loc = match_location::window_title; break;
            case kind_process: r.loc = match_location::process_name; break;
            default: return nullopt;
        }
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
        ::KillTimer(wnd, SHIFT_TIMER);
        set_cloaked(true);
        // Finish our own state before the callback: launching can show an error box whose message loop
        // may run a new show() for the next link, which must not be undone afterwards.
        done_fn d = std::move(done);
        done = nullptr;
        choices.clear();
        const click_payload final_cp = cp;
        ::PostMessageW(wnd, WM_APP_PARK, 0, 0);
        if(d) d(choice, final_cp, rule);
    }

    void picker::on_command(int id, int code) {
        // a click on an owner-drawn button moves the focus to it, and Esc, Enter or digits would then
        // go nowhere; the picker keeps the keyboard
        const bool button = id == ID_TRACKERS || id == ID_RULE || id == ID_REMEMBER;
        if(button && (code == BN_CLICKED || code == BN_DOUBLECLICKED)) ::SetFocus(wnd);
        if(id == ID_REMEMBER_KIND && code == CBN_CLOSEUP) ::PostMessageW(wnd, WM_APP_FOCUS_SELF, 0, 0);
        switch(id) {
            case ID_TRACKERS:
                set_clipboard_text(wnd, cp.raw_url);
                break;
            case ID_RULE:
            case ID_REMEMBER:
                set_remember(!remember, true);
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
                host::show_config();
                break;
            case ID_REMEMBER_KIND:
                if(code == CBN_SELCHANGE && !value_edited) fill_rule_value();
                break;
            case ID_REMEMBER_VALUE:
                if(code == EN_CHANGE && !filling_value) value_edited = true;
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
        bool on = di.CtlID == ID_RULE && remember;
        COLORREF bg = on ? p.toggle_on : (pressed ? p.active : p.bg);
        ::FillRect(hdc, &rc, cached_brush(bg));

        if(di.CtlID == ID_REMEMBER) {
            // check box: a square, a tick when on, then the label
            const int sz = scale(14, dpi);
            RECT box{rc.left + scale(2, dpi), rc.top + (rc.bottom - rc.top - sz) / 2, 0, 0};
            box.right = box.left + sz;
            box.bottom = box.top + sz;
            HBRUSH fill = cached_brush(remember ? p.toggle_on : p.edit_bg);
            HGDIOBJ old_b = ::SelectObject(hdc, fill);
            HPEN pen = ::CreatePen(PS_SOLID, 1, p.border);
            HGDIOBJ old_p = ::SelectObject(hdc, pen);
            ::RoundRect(hdc, box.left, box.top, box.right, box.bottom, scale(3, dpi), scale(3, dpi));
            ::SelectObject(hdc, old_p);
            ::SelectObject(hdc, old_b);
            ::DeleteObject(pen);
            ::SetBkMode(hdc, TRANSPARENT);
            ::SetTextColor(hdc, p.text);
            if(remember) {
                HGDIOBJ old_f = ::SelectObject(hdc, icon_font(dpi, 8));
                ::DrawTextW(hdc, GLYPH_CHECK, -1, &box, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                ::SelectObject(hdc, old_f);
            }
            RECT text = rc;
            text.left = box.right + scale(6, dpi);
            HGDIOBJ old_f = ::SelectObject(hdc, ui_font(dpi));
            ::DrawTextW(hdc, L"Remember", -1, &text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            ::SelectObject(hdc, old_f);
            return;
        }

        const wchar_t* glyph{nullptr};
        HFONT font = icon_font(dpi);
        COLORREF fg = p.text;
        switch(di.CtlID) {
            case ID_TRACKERS: glyph = GLYPH_SHIELD; fg = RGB(220, 80, 60); break;
            case ID_RULE: glyph = GLYPH_RULE; break;
            case ID_COPY: glyph = GLYPH_COPY; break;
            case ID_EMAIL: glyph = GLYPH_MAIL; break;
            case ID_SETTINGS: glyph = GLYPH_SETTINGS; break;
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
        for(const RECT& box : {url_box, rule_box}) {
            if(box.right <= box.left) continue;
            HGDIOBJ old_b = ::SelectObject(hdc, cached_brush(pal.edit_bg));
            HPEN pen = ::CreatePen(PS_SOLID, 1, pal.border);
            HGDIOBJ old_p = ::SelectObject(hdc, pen);
            const int r = scale(6, dpi);
            ::RoundRect(hdc, box.left, box.top, box.right, box.bottom, r, r);
            ::SelectObject(hdc, old_p);
            ::SelectObject(hdc, old_b);
            ::DeleteObject(pen);
        }
        paint_tiles(hdc);

        if(!section_open && !choices.empty()) {
            RECT hint{pad, footer_y, w - pad, footer_y + footer_h};
            ::SetBkMode(hdc, TRANSPARENT);
            ::SetTextColor(hdc, pal.dim);
            HGDIOBJ old_f = ::SelectObject(hdc, ui_font(dpi, -1));
            ::DrawTextW(hdc, L"Hold Shift and click a profile to remember this host for it (or press R)", -1, &hint,
                DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            ::SelectObject(hdc, old_f);
        }

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
            case WM_TIMER:
                if(w == SHIFT_TIMER) poll_shift();
                return 0;
            case WM_APP_FOCUS_SELF:
                ::SetFocus(wnd);
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
                        set_remember(!remember, true);
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
