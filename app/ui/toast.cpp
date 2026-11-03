#include "toast.h"
#include <format>
#include <algorithm>
#include "win32ui.h"
#include "icons.h"
#include "globals.h"
#include "common/url.h"
#include "common/str.h"

using namespace std;
using namespace grey::common;

namespace bt::ui {

    namespace {
        constexpr wchar_t CLASS_NAME[] = L"BrowserTamer.Toast";
        constexpr UINT_PTR TIMER_ID = 1;

        float max_alpha() {
            float o = g_state.toast.opacity;
            if(o > 1.0f) o /= 255.0f;   // older configs stored 50..255
            return clamp(o, 0.2f, 1.0f);
        }
    }

    void toast::create() {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.style = CS_DROPSHADOW;
        wc.lpfnWndProc = wndproc;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = CLASS_NAME;
        ::RegisterClassExW(&wc);

        wnd = ::CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            CLASS_NAME, L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, wc.hInstance, this);
    }

    void toast::show(const click_payload& cp, const browser& br, const browser_profile& pr, const match_rule& rule) {
        b = br;
        p = pr;
        line1 = cp.process_description.empty() ? cp.process_name : cp.process_description;
        if(line1.empty()) line1 = "Unknown";
        url u{cp.url};
        line2 = u.host.empty() ? u.path : u.host;
        rule_text = rule.is_fallback ? "" : format("rule: {}", rule.to_string(false));
        source_path = cp.process_path;
        trackers = cp.trackers_removed;
        dark = system_uses_dark_theme();
        hovered = false;

        RECT work = cursor_work_area(&dpi);
        HFONT font = ui_font(dpi), bold = ui_font(dpi, 0, true);

        auto width_of = [](HFONT f, const string& s) {
            HDC hdc = ::GetDC(nullptr);
            HGDIOBJ old = ::SelectObject(hdc, f);
            SIZE sz{};
            wstring w = wide(s);
            ::GetTextExtentPoint32W(hdc, w.c_str(), static_cast<int>(w.size()), &sz);
            ::SelectObject(hdc, old);
            ::ReleaseDC(nullptr, hdc);
            return static_cast<int>(sz.cx);
        };

        const int pad = scale(10, dpi), icon = scale(20, dpi), gap = scale(6, dpi);
        string extra = rule_text;
        if(trackers > 0) extra += format("{}{} {} removed", extra.empty() ? "" : "  ·  ", trackers, str::pluralize(trackers, "tracker"));
        int text_w = max({width_of(font, line1), width_of(bold, line2), width_of(font, extra)});
        int w = min(pad + icon + gap + text_w + pad, static_cast<int>((work.right - work.left) * 0.4));
        int rows = extra.empty() ? 2 : 3;
        int h = pad + icon * rows + gap * (rows - 1) + pad;
        int margin = scale(12, dpi);

        ::SetWindowPos(wnd, HWND_TOPMOST, work.right - w - margin, work.bottom - h - margin, w, h,
            SWP_NOACTIVATE | SWP_SHOWWINDOW);
        apply_round_corners(wnd);
        set_alpha(0);
        ::InvalidateRect(wnd, nullptr, TRUE);

        st = stage::fade_in;
        stage_started = ::GetTickCount();
        ::SetTimer(wnd, TIMER_ID, 15, nullptr);
    }

    void toast::set_alpha(float a) {
        ::SetLayeredWindowAttributes(wnd, 0, static_cast<BYTE>(clamp(a, 0.0f, 1.0f) * 255), LWA_ALPHA);
    }

    void toast::tick() {
        const DWORD now = ::GetTickCount();
        const float anim_ms = max(1.0f, g_state.toast.anim_duration * 1000.0f);
        const float elapsed = static_cast<float>(now - stage_started);

        switch(st) {
            case stage::fade_in:
                set_alpha(max_alpha() * min(1.0f, elapsed / anim_ms));
                if(elapsed >= anim_ms) {
                    st = stage::visible;
                    stage_started = now;
                }
                break;
            case stage::visible:
                if(hovered) stage_started = now;
                else if(elapsed >= g_state.toast.visible_seconds * 1000.0f) {
                    st = stage::fade_out;
                    stage_started = now;
                }
                break;
            case stage::fade_out:
                if(hovered) {
                    st = stage::visible;
                    stage_started = now;
                    set_alpha(max_alpha());
                    break;
                }
                set_alpha(max_alpha() * (1.0f - min(1.0f, elapsed / anim_ms)));
                if(elapsed >= anim_ms) {
                    st = stage::hidden;
                    ::KillTimer(wnd, TIMER_ID);
                    ::ShowWindow(wnd, SW_HIDE);
                }
                break;
            case stage::hidden:
                ::KillTimer(wnd, TIMER_ID);
                break;
        }
    }

    void toast::paint(HDC hdc) {
        RECT rc;
        ::GetClientRect(wnd, &rc);
        const COLORREF bg = dark ? RGB(32, 32, 32) : RGB(249, 249, 249);
        const COLORREF fg = dark ? RGB(240, 240, 240) : RGB(26, 26, 26);
        const COLORREF dim = dark ? RGB(160, 160, 160) : RGB(96, 96, 96);
        const COLORREF border = dark ? RGB(80, 80, 80) : RGB(200, 200, 200);

        HBRUSH bgb = ::CreateSolidBrush(bg);
        ::FillRect(hdc, &rc, bgb);
        ::DeleteObject(bgb);
        if(int bw = scale(g_state.toast.border_width, dpi); bw > 0) {
            HBRUSH bb = ::CreateSolidBrush(border);
            for(RECT e : {RECT{0, 0, rc.right, bw}, RECT{0, rc.bottom - bw, rc.right, rc.bottom},
                          RECT{0, 0, bw, rc.bottom}, RECT{rc.right - bw, 0, rc.right, rc.bottom}})
                ::FillRect(hdc, &e, bb);
            ::DeleteObject(bb);
        }

        const int pad = scale(10, dpi), icon = scale(20, dpi), gap = scale(6, dpi);
        const int text_x = pad + icon + gap;
        ::SetBkMode(hdc, TRANSPARENT);

        auto text = [&](HFONT f, COLORREF c, const string& s, int y) {
            HGDIOBJ old = ::SelectObject(hdc, f);
            ::SetTextColor(hdc, c);
            RECT r{text_x, y, rc.right - pad, y + icon};
            wstring w = wide(s);
            ::DrawTextW(hdc, w.c_str(), -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            ::SelectObject(hdc, old);
        };

        int y = pad;
        icons::draw(hdc, icons::path_bitmap(source_path.empty() ? "logo" : source_path, icon, false), pad, y);
        text(ui_font(dpi), dim, line1, y);

        y += icon + gap;
        if(b && p) icons::draw(hdc, icons::profile_bitmap(*b, *p, icon, g_state.icon_overlay), pad, y);
        text(ui_font(dpi, 0, true), fg, line2, y);

        string extra = rule_text;
        if(trackers > 0) extra += format("{}{} {} removed", extra.empty() ? "" : "  ·  ", trackers, str::pluralize(trackers, "tracker"));
        if(!extra.empty()) {
            y += icon + gap;
            text(ui_font(dpi, -1), dim, extra, y);
        }
    }

    LRESULT CALLBACK toast::wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
        toast* self{nullptr};
        if(m == WM_NCCREATE) {
            self = static_cast<toast*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
            self->wnd = h;
            ::SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<toast*>(::GetWindowLongPtrW(h, GWLP_USERDATA));
        }
        return self ? self->handle(m, w, l) : ::DefWindowProcW(h, m, w, l);
    }

    LRESULT toast::handle(UINT m, WPARAM w, LPARAM l) {
        switch(m) {
            case WM_MOUSEACTIVATE:
                return MA_NOACTIVATE;
            case WM_PAINT: {
                PAINTSTRUCT ps;
                HDC hdc = ::BeginPaint(wnd, &ps);
                paint(hdc);
                ::EndPaint(wnd, &ps);
                return 0;
            }
            case WM_TIMER:
                tick();
                return 0;
            case WM_MOUSEMOVE:
                if(!hovered) {
                    hovered = true;
                    TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, wnd, 0};
                    ::TrackMouseEvent(&tme);
                }
                return 0;
            case WM_MOUSELEAVE:
                hovered = false;
                return 0;
            case WM_LBUTTONUP:
                st = stage::hidden;
                ::KillTimer(wnd, TIMER_ID);
                ::ShowWindow(wnd, SW_HIDE);
                return 0;
            default:
                break;
        }
        return ::DefWindowProcW(wnd, m, w, l);
    }
}
